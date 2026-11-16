#include "alerts.h"
#include "alert_policy.h"
#include "cJSON.h"
#include "emergency_policy.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "geolocate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"
#include "sms.h"
#include "vision_service.h"
#include "snapshot_wait.h"
#include "wifi_sync.h"
#include <atomic>
#include <mutex>
#include <string>
#include <time.h>
#include <memory>
#include <vector>
namespace alerts {
namespace {
using namespace alert_policy;
struct Job {
    Kind kind;
    uint32_t event;
    sensors::Snapshot s;
};
QueueHandle_t queue;
Policy policy;
emergency_policy::Policy emergency;
std::atomic<uint32_t> currentEvent{0}, ackEvent{0};
std::atomic<bool> acknowledged{false}, forwarded{false};
std::atomic<uint32_t> sent{0}, failed{0}, dropped{0};
std::atomic<bool> internet{false};
std::atomic<int> httpStatus{0}, networkError{0};
int64_t offset = 0;
std::string bot, pollBot;
std::mutex botLock;
std::string token() {
    std::lock_guard<std::mutex> guard(botLock);
    return bot;
}
uint32_t tokenHash(const std::string &t) {
    uint32_t h = 2166136261u;
    for (unsigned char c : t)
        h = (h ^ c) * 16777619u;
    return h;
}
void loadOffset() {
    nvs_handle_t h;
    if (nvs_open("fg-bot", NVS_READONLY, &h) != ESP_OK) {
        offset = 0;
        return;
    }
    uint32_t hash = 0;
    if (nvs_get_u32(h, "hash", &hash) != ESP_OK || hash != tokenHash(pollBot) ||
        nvs_get_i64(h, "offset", &offset) != ESP_OK)
        offset = 0;
    nvs_close(h);
}
void saveOffset() {
    nvs_handle_t h;
    if (nvs_open("fg-bot", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u32(h, "hash", tokenHash(pollBot));
    nvs_set_i64(h, "offset", offset);
    nvs_commit(h);
    nvs_close(h);
}

struct Reply {
    std::string body;
    bool overflow = false;
};
esp_err_t on_http(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        auto *r = (Reply *)e->user_data;
        if (r->body.size() + e->data_len > 32768) {
            r->overflow = true;
            return ESP_FAIL;
        }
        r->body.append((const char *)e->data, e->data_len);
    }
    return ESP_OK;
}
cJSON *request(const char *method, const std::string &data, const char *type = "application/json") {
    auto currentToken = token();
    if (!wifi_sync::connected() || time(nullptr) < 1700000000 || currentToken.empty()) {
        internet = false;
        return nullptr;
    }
    std::string url = "https://api.telegram.org/bot" + currentToken + "/" + method;
    Reply reply;
    esp_http_client_config_t c = {};
    c.url = url.c_str();
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 5000;
    c.event_handler = on_http;
    c.user_data = &reply;
    c.buffer_size = 2048;
    c.disable_auto_redirect = true;
    auto client = esp_http_client_init(&c);
    if (!client)
        return nullptr;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", type);
    esp_http_client_set_post_field(client, data.data(), data.size());
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    httpStatus=status;networkError=err;
    esp_http_client_cleanup(client);
    cJSON *j = err == ESP_OK && status == 200 && !reply.overflow ? cJSON_Parse(reply.body.c_str()) : nullptr;
    bool ok = j && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok"));
    internet = ok;
    if (!ok) {
        cJSON_Delete(j);
        failed++;
        return nullptr;
    }
    return j;
}
std::string encode(cJSON *j) {
    char *p = cJSON_PrintUnformatted(j);
    std::string b = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(j);
    return b;
}
bool message(int64_t id, const std::string &text) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "chat_id", double(id));
    cJSON_AddStringToObject(j, "text", text.c_str());
    cJSON *r = request("sendMessage", encode(j));
    bool ok = r;
    cJSON_Delete(r);
    if (ok)
        sent++;
    return ok;
}
bool long_message(int64_t id, const std::string &text) {
    size_t pos = 0;
    bool ok = true;
    while (pos < text.size()) {
        size_t end = min(pos + size_t(3500), text.size());
        if (end < text.size()) {
            while (end > pos && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
                --end;
            size_t newline = text.rfind('\n', end);
            if (newline != std::string::npos && newline > pos + 1000)
                end = newline;
        }
        if (!message(id, text.substr(pos, end - pos)))
            ok = false;
        pos = end;
    }
    return ok;
}
// Telegram map pin from the hybrid location (fixed location first, Wi-Fi estimate as fallback).
bool location(int64_t id, const location_policy::Resolved &where) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "chat_id", double(id));
    cJSON_AddNumberToObject(j, "latitude", where.lat);
    cJSON_AddNumberToObject(j, "longitude", where.lng);
    if (where.source == location_policy::WIFI && where.accuracyM > 0)
        cJSON_AddNumberToObject(j, "horizontal_accuracy", where.accuracyM > 1500 ? 1500 : where.accuracyM);
    cJSON *r = request("sendLocation", encode(j));
    bool ok = r;
    cJSON_Delete(r);
    if (ok)
        sent++;
    return ok;
}
bool photo(int64_t id, const std::vector<uint8_t> &jpg) {
    const char *boundary = "FireGuardImageBoundary8f57";
    std::string body = "--" + std::string(boundary) +
                       "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + std::to_string(id) +
                       "\r\n--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"photo\"; "
                       "filename=\"kitchen.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    body.append((const char *)jpg.data(), jpg.size());
    body += "\r\n--" + std::string(boundary) + "--\r\n";
    std::string type = "multipart/form-data; boundary=" + std::string(boundary);
    cJSON *r = request("sendPhoto", body, type.c_str());
    bool ok = r;
    cJSON_Delete(r);
    if (ok)
        sent++;
    return ok;
}
std::string status(const sensors::Snapshot &s) {
    char b[900];
    snprintf(b, sizeof b,
        "%s\nFire %.0f / Gas activity %.0f\nTemperature %.1f C; humidity %.1f%%; pressure %.1f hPa"
        "\nMQ2 AO %.0f mV (ADC %.0f); MQ135 AO %.0f mV (ADC %.0f)"
        "\nRelative deviation: MQ2 %.3f; MQ135 %.3f\nADS1115 %s; MQ2 %s; MQ135 %s"
        "\nCamera %s; buzzer %s; warm-up %s; fault %s; burn-in %.1f h"
        "\nCO unavailable. Prototype relative scores, not ppm or gas identification.",
        STATE_NAME[s.state],s.frs,s.grs,s.tC,s.rh,s.hPa,s.adcMv2*2,s.adcMv2,s.adcMv135*2,s.adcMv135,
        s.dev[0],s.dev[2],s.adcOK?"OK":"fault",s.mq2OK?"OK":"fault",s.mq135OK?"OK":"fault",
        s.cameraValid?"fresh":"asleep/unavailable",s.muted?"muted":"enabled",
        (s.flags&2)?"in progress":"complete",(s.flags&1)?"reported":"none reported",s.burninSec/3600.0);

    return b;
}
std::string text(const Job &j, const settings::Config &c, const location_policy::Resolved &where) {
    if (j.kind == SNAPSHOT)
        return std::string("FireGuard — requested camera snapshot\n") + c.home + "\n" + status(j.s);
    if (j.kind == READINGS)
        return std::string("FireGuard — requested readings\n") + c.home + "\n" + status(j.s);
    const char *lead = "FireGuard update";
    switch (j.kind) {
    case READINGS:
        lead = "FireGuard — requested sensor readings";
        break;
    case WARNING:
        lead = "WARNING: elevated sensor readings. Please check the kitchen and acknowledge.";
        break;
    case EMERGENCY_UNATTENDED:
        lead = "EMERGENCY FORWARD: warning unattended for the configured interval.";
        break;
    case EMERGENCY_HIGH:
        lead = "EMERGENCY FORWARD: high-confidence open flame detected.";
        break;
    case EMERGENCY_TEST:
        lead = "TEST ONLY: emergency recipient delivery check.";
        break;
    case FIRE:
        lead = j.s.cameraValid && j.s.vision.openFlame >= 0.6f
                   ? "FIRE ALERT: camera and scores indicate fire."
                   : "FIRE ALERT: high sensor score; camera has not confirmed flame.";
        break;
    case POSSIBLE:
        lead = "Possible fire: check the kitchen.";
        break;
    case GAS:
        lead = "GAS ALERT: possible gas leak. Leave the area; do not operate switches or create flames.";
        break;
    case CO:
        lead = "CO measurement unavailable in this hardware revision.";
        break;
    case ESCALATE:
        lead = "Persistent fire alarm: call the fire station now.";
        break;
    case ALL_CLEAR:
        lead = "All clear: FireGuard has returned to SAFE. Check the area before returning.";
        break;
    case TEST:
        lead = "TEST ONLY: FireGuard alert delivery check.";
        break;
    default:
        break;
    }
    std::string prefix =
        (j.kind == EMERGENCY_UNATTENDED || j.kind == EMERGENCY_HIGH) && c.emergencyTest ? "TEST MODE — " : "";
    std::string out = prefix + std::string(lead) + "\n" + c.home + "\n" + c.address +
                      "\nLandmark: " + c.landmark + "\n" + status(j.s);
    out += location_policy::telegram_line(where);
    out += "\nFire station: " + std::string(c.station) + " " + c.stationPhone +
           "\nPhone calls are not automatic. Emergency forwarding reaches only the registered Telegram "
           "recipient and, when switched on, the fire-station number by SMS.";
    out += "\nAmbulance / test number: " + std::string(c.ambulancePhone);
    out += "\nEmergency recipient / test number: " + std::string(c.emergencyPhone);
    if (j.event && j.kind != ALL_CLEAR)
        out += "\nResidents: acknowledge with /ack " + std::to_string(j.event);
    out += "\nContacts:";
    for (auto &v : c.contacts)
        if (v.name[0] || v.phone[0])
            out += "\n" + std::string(v.name) + " " + v.phone +
                   (v.address[0] ? " — " + std::string(v.address) : "");
    return out;
}
void poll() {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "offset", double(offset));
    cJSON_AddNumberToObject(j, "timeout", 0);
    cJSON_AddNumberToObject(j, "limit", 8);
    cJSON *a = cJSON_AddArrayToObject(j, "allowed_updates");
    cJSON_AddItemToArray(a, cJSON_CreateString("message"));
    cJSON *r = request("getUpdates", encode(j));
    if (!r)
        return;
    int64_t before = offset;
    cJSON *u;
    cJSON_ArrayForEach(u, cJSON_GetObjectItemCaseSensitive(r, "result")) {
        cJSON *uid = cJSON_GetObjectItemCaseSensitive(u, "update_id");
        if (cJSON_IsNumber(uid))
            offset = max(offset, int64_t(uid->valuedouble) + 1);
        cJSON *m = cJSON_GetObjectItemCaseSensitive(u, "message"),
              *chat = cJSON_GetObjectItemCaseSensitive(m, "chat"),
              *id = cJSON_GetObjectItemCaseSensitive(chat, "id"),
              *type = cJSON_GetObjectItemCaseSensitive(chat, "type"),
              *t = cJSON_GetObjectItemCaseSensitive(m, "text");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(t) || !cJSON_IsString(type) ||
            strcmp(type->valuestring, "private"))
            continue;
        int64_t cid = int64_t(id->valuedouble);
        std::unique_ptr<settings::Config> saved(new settings::Config(settings::get()));
        auto &c=*saved;
        bool known = false;
        for (auto &v : c.contacts)
            if (v.chat == cid)
                known = true;
        std::string cmd = t->valuestring;
        // Global limit bounds both brute-force PIN attempts and reply traffic.
        static uint32_t joinAt = 0, testAt = 0;
        if (cmd.rfind("/join ", 0) == 0 && millis() - joinAt >= 5000) {
            joinAt = millis();
            if (cmd.substr(6) == c.pin) {
                cJSON *from = cJSON_GetObjectItemCaseSensitive(m, "from"),
                      *name = cJSON_GetObjectItemCaseSensitive(from, "first_name");
                bool ok = settings::join(cid, cJSON_IsString(name) ? name->valuestring : "Resident");
                message(cid, ok ? "Registered for FireGuard alerts. Use /status and /test."
                                : "Contact list is full. Ask the installer to remove a contact.");
            }
        } else if (cmd.rfind("/emergency ", 0) == 0 && millis() - joinAt >= 5000) {
            joinAt = millis();
            if (cmd.substr(11) == c.emergencyPin) {
                auto *from = cJSON_GetObjectItemCaseSensitive(m, "from");
                auto *name = cJSON_GetObjectItemCaseSensitive(from, "first_name");
                bool ok = settings::join_emergency(cid, cJSON_IsString(name) ? name->valuestring
                                                                             : "Emergency recipient");
                message(cid, ok ? "Registered as the FireGuard emergency/test recipient. Photos are "
                                  "forwarded only when the installer enables the feature."
                                : "Could not save emergency registration");
            }
        } else if (known && cmd.rfind("/ack ", 0) == 0) {
            char *end = nullptr;
            unsigned long id = strtoul(cmd.c_str() + 5, &end, 10);
            bool ok = end && !*end && id <= UINT32_MAX && acknowledge(uint32_t(id));
            message(cid,
                    ok ? "Acknowledgement received. High-confidence fire can still be forwarded when enabled."
                       : "This event is no longer current. Use /status.");
        } else if (known && cmd == "/status")
            message(cid, status(sensors::snapshot()));
        else if (known && cmd == "/test" && millis() - testAt >= 30000) {
            testAt = millis();
            Job j{TEST, 0, sensors::snapshot()};
            long_message(cid, text(j, c, geolocate::current()));
        }
    }
    if (offset != before)
        saveOffset();
    cJSON_Delete(r);
}
void poll_task(void *) {
    for (;;) {
        auto current = token();
        if (current != pollBot) {
            pollBot = current;
            loadOffset();
        }
        if (!current.empty() && wifi_sync::connected() && time(nullptr) >= 1700000000)
            poll();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
void task(void *) {
    struct Pending {
        Job job;
        uint8_t done[settings::MAX_CONTACTS] = {};
        uint8_t attempts[settings::MAX_CONTACTS][3] = {};
        int phase = 0, index = 0;
        bool deliveryFailed = false;
        uint32_t expires = 0;
        std::vector<uint8_t> jpg;
        SnapshotWait photoWait;
        bool waitingPhoto=false, requestedPhoto=false;
        settings::Config config;
        location_policy::Resolved where; // captured when the alert was queued
    };
    std::vector<Pending> pending;
    uint32_t lastSend = 0;
    for (;;) {
        std::unique_ptr<settings::Config> saved(new settings::Config(settings::get()));
        auto &c=*saved;
        {
            std::lock_guard<std::mutex> guard(botLock);
            if (bot != c.token) {
                bot = c.token;
                pending.clear();
            }
        }
        Job j;
        while (xQueueReceive(queue, &j, 0) == pdTRUE) {
            if (pending.size() >= 12) {
                dropped++;
                continue;
            }
            std::unique_ptr<Pending> allocated(new Pending);
            auto &p=*allocated;
            p.job = j;
            p.config = c;
            p.where = location_policy::resolve(c.hasLocation, c.latitude, c.longitude, geolocate::health().fix, time(nullptr));
            p.expires = millis() + 900000UL;
            if (j.kind == TEST || j.kind == EMERGENCY_TEST || j.kind == SNAPSHOT) {
                p.photoWait.start(millis()); p.waitingPhoto=true; p.requestedPhoto=true;
                vision_service::request_capture();
            }
            if (j.kind == FIRE || j.kind == UPDATE || j.kind == TEST || j.kind == EMERGENCY_HIGH ||
                j.kind == EMERGENCY_UNATTENDED || j.kind == EMERGENCY_TEST)
                if (!p.waitingPhoto) vision_service::photo(p.jpg);
            if (j.kind == EMERGENCY_HIGH || j.kind == EMERGENCY_UNATTENDED || j.kind == EMERGENCY_TEST)
                pending.insert(pending.begin(), std::move(p));
            else
                pending.push_back(std::move(p));
        }
        if (!token().empty() && wifi_sync::connected() && time(nullptr) >= 1700000000) {
            if (!pending.empty() && millis() - lastSend >= 1000) {
                auto &p = pending.front();
                bool isEmergency = p.job.kind == EMERGENCY_HIGH || p.job.kind == EMERGENCY_UNATTENDED ||
                                   p.job.kind == EMERGENCY_TEST;
                if (isEmergency && p.job.kind != EMERGENCY_TEST) {
                    auto options = settings::alert_options();
                    bool obsolete = p.job.event != currentEvent.load() || !options.enabled ||
                                    p.config.emergencyChat != options.chat ||
                                    (p.job.kind == EMERGENCY_UNATTENDED && acknowledged.load());
                    if (obsolete) {
                        pending.erase(pending.begin());
                        continue;
                    }
                }
                if (p.waitingPhoto) {
                    auto image=vision_service::health();
                    auto result=p.photoWait.result(millis(),image.imageAt);
                    if (result==SnapshotWait::FRESH && vision_service::photo(p.jpg,20000)) p.waitingPhoto=false;
                    else if(result==SnapshotWait::EXPIRED) p.waitingPhoto=false;
                }
                auto body = text(p.job, p.config, p.where);
                if (isEmergency && p.jpg.empty())
                    body += "\nCamera image unavailable; inspect the home and verify the alarm.";
                // One delivery per iteration keeps emergency jobs responsive. Text to
                // all contacts is completed before photo/location phases begin.
                bool attempted = false;
                while (p.phase < 3 && !attempted) {
                    if (p.phase==1 && p.waitingPhoto) break;
                    for (int scanned = 0; scanned < settings::MAX_CONTACTS; scanned++) {
                        int i = (p.index + scanned) % settings::MAX_CONTACTS;
                        int64_t id =
                            isEmergency ? (i == 0 ? p.config.emergencyChat : 0) : p.config.contacts[i].chat;
                        uint8_t bit = 1 << p.phase;
                        if (!id || (p.done[i] & bit))
                            continue;
                        bool ok = p.phase == 0   ? long_message(id, body)
                                  : p.phase == 1 ? (!p.jpg.empty() ? photo(id,p.jpg) :
                                    !p.requestedPhoto || long_message(id,std::string("Snapshot unavailable: ")+
                                        (vision_service::health().reason[0] ? vision_service::health().reason : "camera capture timed out")))
                                                 : (!p.where.valid || location(id, p.where));
                        if (ok)
                            p.done[i] |= bit;
                        else if (++p.attempts[i][p.phase] >= 3) {
                            p.done[i] |= bit;
                            p.deliveryFailed = true;
                        }
                        p.index = (i + 1) % settings::MAX_CONTACTS;
                        attempted = true;
                        break;
                    }
                    if (!attempted) {
                        ++p.phase;
                        p.index = 0;
                    }
                }
                lastSend = millis();
                if (p.phase >= 3 || int32_t(millis() - p.expires) >= 0) {
                    if (p.deliveryFailed || p.phase < 3)
                        dropped++;
                    pending.erase(pending.begin());
                }
            }
        } else
            internet = false;
        // Offline jobs expire after 15 minutes so old alerts do not arrive indefinitely.
        while (!pending.empty() && int32_t(millis() - pending.front().expires) >= 0) {
            pending.erase(pending.begin());
            dropped++;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
} // namespace
void start() {
    emergency = emergency_policy::Policy(esp_random());
    queue = xQueueCreate(12, sizeof(Job));
    configASSERT(queue);
    configASSERT(xTaskCreate(task, "telegram", 12288, nullptr, 2, nullptr) == pdPASS);
    configASSERT(xTaskCreate(poll_task, "telegram-poll", 12288, nullptr, 2, nullptr) == pdPASS);
}
void observe(const sensors::Snapshot &s) {
    if (!queue)
        return;
    auto options = settings::alert_options();
    bool high = s.cameraValid && s.vision.conf >= 0.4f && s.vision.openFlame >= options.fireConfidence;
    emergency.track(severityOf(SysState(s.state)) >= 1, high, s.state == ST_SAFE, s.ms);
    // No Wi-Fi scans / location lookups while anything is alarming.
    geolocate::set_alarm(severityOf(SysState(s.state)) >= 1 || high);
    uint32_t requested = ackEvent.exchange(0);
    if (requested)
        emergency.acknowledge(requested);
    currentEvent = emergency.event();
    acknowledged = emergency.acknowledged();
    forwarded = emergency.forwarded();
    Input i{s.state, s.cameraValid && s.vision.openFlame >= 0.6f, s.muted, s.ms};
    policy.track(i);
    Intent n = policy.next(i);
    if (n.kind != NONE) {
        Job j{n.kind, currentEvent.load(), s};
        if (xQueueSend(queue, &j, 0) == pdTRUE) {
            policy.accepted(n, i);
            // SMS runs in its own task/queue; this call only enqueues (FIRE/GAS/CO/ESCALATE/ALL_CLEAR).
            sms::notify(n.kind, n.event, j.event, i.flame);
        }
    }
    // An emergency forward goes to the Telegram emergency recipient and/or, when switched on, the
    // fire-station number by SMS (with the address and the hybrid map location).
    bool station = sms::station_ready();
    auto reason = emergency.next(options.enabled, options.recipient || station, high,
                                 options.unattendedSeconds * 1000UL, s.ms);
    if (reason != emergency_policy::NONE) {
        Job j{reason == emergency_policy::HIGH_FIRE ? EMERGENCY_HIGH : EMERGENCY_UNATTENDED,
              currentEvent.load(), s};
        static uint32_t telegramQueuedEvent=0;
        bool telegramOK=!options.recipient || telegramQueuedEvent==j.event;
        if(!telegramOK && xQueueSend(queue,&j,0)==pdTRUE){telegramQueuedEvent=j.event;telegramOK=true;}
        bool smsOK=!station;
        if(station){
            auto where=geolocate::current();
            smsOK=sms::notify_station(j.event,reason==emergency_policy::HIGH_FIRE?location_policy::STATION_FIRE:location_policy::STATION_UNATTENDED,options.test,where);
        }
        if(telegramOK && smsOK){emergency.accepted();forwarded=true;}

    }
}
bool acknowledge(uint32_t event) {
    if (!event || event != currentEvent.load())
        return false;
    ackEvent = event;
    return true;
}
bool test_emergency() {
    if (!queue || !settings::alert_options().recipient)
        return false;
    static std::atomic<uint32_t> last{0};
    uint32_t now = millis(), prev = last.load();
    if (prev && now - prev < 30000)
        return false;
    if (!last.compare_exchange_strong(prev, now))
        return false;
    Job j{EMERGENCY_TEST, 0, sensors::snapshot()};
    return xQueueSend(queue, &j, 0) == pdTRUE;
}
bool readings() {
    if (!queue)
        return false;
    static std::atomic<uint32_t> last{0};
    uint32_t now = millis(), prev = last.load();
    if (prev && now - prev < 30000)
        return false;
    if (!last.compare_exchange_strong(prev, now))
        return false;
    Job j{READINGS, 0, sensors::snapshot()};
    return xQueueSend(queue, &j, 0) == pdTRUE;
}
bool test() {
    if (!queue)
        return false;
    static std::atomic<uint32_t> last{0};
    uint32_t now = millis(), prev = last.load();
    if (prev && now - prev < 30000)
        return false;
    if (!last.compare_exchange_strong(prev, now))
        return false;
    Job j{TEST, 0, sensors::snapshot()};
    return xQueueSend(queue, &j, 0) == pdTRUE;
}
bool snapshot() {
    if (!queue) return false;
    static std::atomic<uint32_t> last{0};
    uint32_t now=millis(),prev=last.load();
    if(prev && now-prev<30000) return false;
    if(!last.compare_exchange_strong(prev,now)) return false;
    Job j{SNAPSHOT,0,sensors::snapshot()};
    return xQueueSend(queue,&j,0)==pdTRUE;
}
Health health() {
    Health h;
    h.httpStatus=httpStatus;h.networkError=networkError;
    h.sent = sent;
    h.failed = failed;
    h.dropped = dropped;
    h.internet = internet;
    h.registered = settings::registered_count();
    h.event = currentEvent;
    h.acknowledged = acknowledged;
    h.forwarded = forwarded;
    return h;
}
} // namespace alerts
