#include "sms.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings_store.h"
#include "settings.h"
#include "sms_policy.h"
#include "wifi_sync.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string.h>
#include <time.h>
#include <vector>

namespace sms {
namespace {
constexpr const char *TAG = "sms";
constexpr const char *SEND_URL = "https://api.textbee.dev/api/v1/gateway/send-sms";
constexpr uint32_t JOB_EXPIRY_MS = 600000; // undelivered alerts older than 10 min are dropped
constexpr int MAX_ATTEMPTS = 4;

// Stored in its own NVS key so the main settings blob (and its saved data) is unchanged.
struct Options {
    uint32_t version = 1;
    bool enabled = false;
    char apiKey[128] = {};
    char deviceId[64] = {};
};
// Fire-station SMS switch: its own NVS key so the existing "opts" blob keeps its size and version.
struct StationOptions {
    uint32_t version = 1;
    bool enabled = false;
};
struct Job {
    alert_policy::Kind kind;
    uint32_t event;
    bool flame, test;
    uint32_t queuedAt;
    char stationPhone[20]={},stationMessage[307]={};
    bool station = false; // to the fire-station number instead of residents
    location_policy::StationReason reason = location_policy::STATION_FIRE;
    bool testMode = false;
    location_policy::Resolved where; // location captured when the alarm fired
};
Job make_job(alert_policy::Kind kind, uint32_t event, bool flame, bool test, uint32_t at) {
    Job j;
    j.kind = kind;
    j.event = event;
    j.flame = flame;
    j.test = test;
    j.queuedAt = at;
    return j;
}

Options options;
StationOptions stationOptions;
std::mutex lock;
QueueHandle_t queue;
sms_policy::Limiter limiter;
std::mutex limiterLock;
std::atomic<uint32_t> sent{0}, failed{0}, skipped{0};
std::atomic<int> lastStatus{0};
std::atomic<uint32_t> stationSent{0}, stationEvent{0};

uint32_t now_ms() { return uint32_t(pdTICKS_TO_MS(xTaskGetTickCount())); }

Options get_options() {
    std::lock_guard<std::mutex> g(lock);
    return options;
}

bool persist(const Options &o, const char *key = "opts", size_t size = 0, const void *data = nullptr) {
    nvs_handle_t h;
    if (settings_store::open_write("fg-sms", &h) != ESP_OK)
        return false;
    esp_err_t e = data ? nvs_set_blob(h, key, data, size) : nvs_set_blob(h, "opts", &o, sizeof o);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// API keys and device IDs are plain tokens; reject anything that could break the HTTP header.
bool token_ok(const char *s) {
    for (const char *p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' ||
              *p == '_' || *p == '.'))
            return false;
    return true;
}

std::string station_number(const settings::Config &c) { return sms_policy::normalise_phone(c.stationPhone); }

// First resident with a usable number: the fire station's call-back contact.
std::string callback_number(const settings::Config &c) {
    for (auto &v : c.contacts)
        if (v.phone[0] && !sms_policy::normalise_phone(v.phone).empty())
            return v.phone;
    return "";
}

std::vector<std::string> recipients(const settings::Config &c) {
    std::vector<std::string> out;
    for (auto &v : c.contacts) {
        if (!v.phone[0])
            continue;
        auto n = sms_policy::normalise_phone(v.phone);
        bool duplicate = false;
        for (auto &e : out)
            duplicate |= e == n;
        if (!n.empty() && !duplicate)
            out.push_back(n);
    }
    return out;
}

// Returns the HTTP status (0 on a network/TLS error).
int post(const Options &o, const std::vector<std::string> &to, const std::string &message) {
    cJSON *j = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(j, "recipients");
    for (auto &n : to)
        cJSON_AddItemToArray(list, cJSON_CreateString(n.c_str()));
    cJSON_AddStringToObject(j, "message", message.c_str());
    if (o.deviceId[0])
        cJSON_AddStringToObject(j, "deviceId", o.deviceId);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body)
        return 0;
    esp_http_client_config_t c = {};
    c.url = SEND_URL;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 10000;
    c.buffer_size = 2048;
    c.disable_auto_redirect = true;
    auto client = esp_http_client_init(&c);
    int status = 0;
    if (client) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "x-api-key", o.apiKey);
        esp_http_client_set_post_field(client, body, strlen(body));
        esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    }
    cJSON_free(body);
    return status;
}

void task(void *) {
    std::vector<Job> pending;
    uint32_t retryAt = 0;
    int attempts = 0;
    for (;;) {
        Job j;
        while (pending.size() < 8 && xQueueReceive(queue, &j, pending.empty() ? pdMS_TO_TICKS(1000) : 0) == pdTRUE)
            pending.push_back(j);
        while (!pending.empty() && now_ms() - pending.front().queuedAt >= JOB_EXPIRY_MS) {
            ESP_LOGW(TAG, "Dropping SMS for event %lu: not sent within 10 minutes",
                     (unsigned long)pending.front().event);
            pending.erase(pending.begin());
            failed++;
            attempts = 0;
        }
        // TLS certificate checks need the real time (SNTP) as well as Wi-Fi.
        if (pending.empty() || !wifi_sync::connected() || time(nullptr) < 1700000000 ||
            int32_t(now_ms() - retryAt) < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        Job job = pending.front();
        Options o = get_options();
        if (!o.enabled || !o.apiKey[0]) {
            pending.erase(pending.begin());
            continue;
        }
        std::unique_ptr<settings::Config> c(new settings::Config(settings::get()));
        std::vector<std::string> to;
        std::string message;
        if (job.station) {
            bool stationEnabled;
            {std::lock_guard<std::mutex> g(lock);stationEnabled=stationOptions.enabled;}
            if(!stationEnabled || (!job.test && !c->emergencyEnabled)){
                pending.erase(pending.begin());skipped++;continue;
            }
            if(job.stationPhone[0])to.push_back(job.stationPhone);
            message=job.stationMessage;
        } else {
            to = recipients(*c);
            message = sms_policy::text(job.kind, c->home, job.event, job.flame, job.test);
        }
        if (to.empty()) {
            ESP_LOGW(TAG, "%s", job.station ? "Fire-station number is not a full phone number; SMS skipped"
                                      : "No resident has a valid phone number; SMS skipped");
            pending.erase(pending.begin());
            skipped++;
            continue;
        }
        c.reset();
        int status = post(o, to, message);
        lastStatus = status;
        if (status == 200 || status == 201) {
            ESP_LOGI(TAG, "SMS accepted by textbee for %u recipient(s)%s", unsigned(to.size()),
                     job.station ? " (fire station)" : "");
            sent += to.size();
            if (job.station)
                stationSent++;
            pending.erase(pending.begin());
            attempts = 0;
            retryAt = now_ms() + 1000;
            continue;
        }
        // 400 = phone offline/disabled, 401 = bad API key, 429 = plan limit: retrying will not help.
        bool permanent = status == 0 || status >= 500 || (status >= 400 && status < 500); // ambiguous requests are not replayed: provider may have accepted them
        ESP_LOGW(TAG, "textbee send failed: HTTP %d%s", status,
                 status == 401 ? " (check API key)"
                 : status == 429 ? " (daily/monthly SMS limit reached)"
                 : status == 400 ? " (is the textbee app running and enabled on the phone?)"
                                 : "");
        if (permanent || ++attempts >= MAX_ATTEMPTS) {
            failed++;
            pending.erase(pending.begin());
            attempts = 0;
        } else
            retryAt = now_ms() + 5000u * attempts; // 5 s, 10 s, 15 s
    }
}

bool enqueue(const Job &j) {
    if (!queue)
        return false;
    if (xQueueSend(queue, &j, 0) != pdTRUE) {
        skipped++;
        return false;
    }
    return true;
}
} // namespace

void init() {
    {
        Options o;
        size_t n = sizeof o;
        if (settings_store::read_blob("fg-sms", "opts", &o, &n) == ESP_OK && n == sizeof o && o.version == 1) {
            o.apiKey[sizeof o.apiKey - 1] = 0;
            o.deviceId[sizeof o.deviceId - 1] = 0;
            std::lock_guard<std::mutex> g(lock);
            options = o;
        }
        StationOptions so;
        n = sizeof so;
        if (settings_store::read_blob("fg-sms", "station", &so, &n) == ESP_OK && n == sizeof so && so.version == 1) {
            std::lock_guard<std::mutex> g(lock);
            stationOptions = so;
        }
    }
}

void start() {
    queue = xQueueCreate(8, sizeof(Job));
    configASSERT(queue);
    // Settings and queued message buffers are heap-resident; keep TLS stack in internal RAM.
    configASSERT(xTaskCreate(task, "sms", 8192, nullptr, 2, nullptr) == pdPASS);
}

void notify(alert_policy::Kind kind, uint32_t policyEvent, uint32_t shownEvent, bool flame) {
    if (!sms_policy::wants_sms(kind))
        return;
    {
        std::lock_guard<std::mutex> g(lock);
        if (!options.enabled || !options.apiKey[0])
            return;
    }
    uint32_t now = now_ms();
    std::lock_guard<std::mutex> g(limiterLock);
    if (!limiter.allow(kind, policyEvent, now)) {
        if (kind != alert_policy::ALL_CLEAR)
            skipped++;
        return;
    }
    if (enqueue(make_job(kind, shownEvent, flame, false, now)))
        limiter.accepted(kind, policyEvent, now);
}

bool test() {
    {
        std::lock_guard<std::mutex> g(lock);
        if (!options.enabled || !options.apiKey[0])
            return false;
    }
    static std::atomic<uint32_t> last{0};
    uint32_t now = now_ms(), prev = last.load();
    if (prev && now - prev < 30000)
        return false;
    if (!last.compare_exchange_strong(prev, now))
        return false;
    return enqueue(make_job(alert_policy::TEST, 0, false, true, now));
}

// Called twice a second from alerts::observe(): the fire-station number check (which copies the
// settings) is cached for 5 s.
bool station_ready() {
    {
        std::lock_guard<std::mutex> g(lock);
        if (!options.enabled || !options.apiKey[0] || !stationOptions.enabled)
            return false;
    }
    static std::atomic<uint32_t> checkedAt{0};
    static std::atomic<bool> numberOk{false};
    uint32_t now = now_ms(), at = checkedAt.load();
    if (!at || now - at >= 5000) {
        std::unique_ptr<settings::Config> c(new settings::Config(settings::get()));
        numberOk = !station_number(*c).empty();
        checkedAt = now ? now : 1;
    }
    return numberOk;
}

bool notify_station(uint32_t event, location_policy::StationReason why, bool testMode,
                    const location_policy::Resolved &where) {
    if (!event || !station_ready())
        return false;
    std::unique_ptr<settings::Config> saved(new settings::Config(settings::get()));
    if(station_number(*saved).empty())return false;
    uint32_t prev = stationEvent.load();
    if(prev==event)return true; // idempotent: this event was already accepted by the queue
    if(!stationEvent.compare_exchange_strong(prev,event))return false; // once per emergency event
    Job j = make_job(alert_policy::FIRE, event, false, false, now_ms());
    j.station = true;
    j.reason = why;
    j.testMode = testMode;
    j.where = where;
    strlcpy(j.stationPhone,station_number(*saved).c_str(),sizeof j.stationPhone);
    auto message=location_policy::station_text(j.reason,j.testMode,saved->home,saved->address,saved->landmark,callback_number(*saved),where,j.event,saved->addressFields.postalCode);
    strlcpy(j.stationMessage,message.c_str(),sizeof j.stationMessage);
    if (!enqueue(j)) {
        stationEvent = prev; // let the caller retry
        return false;
    }
    return true;
}

bool test_station(const location_policy::Resolved &where) {
    if (!station_ready())
        return false;
    static std::atomic<uint32_t> last{0};
    uint32_t now = now_ms(), prev = last.load();
    if (prev && now - prev < 30000)
        return false;
    if (!last.compare_exchange_strong(prev, now))
        return false;
    Job j = make_job(alert_policy::TEST, 0, false, true, now);
    j.station = true;
    j.reason = location_policy::STATION_TEST;
    j.where = where;
    std::unique_ptr<settings::Config> saved(new settings::Config(settings::get()));
    strlcpy(j.stationPhone,station_number(*saved).c_str(),sizeof j.stationPhone);
    auto message=location_policy::station_text(j.reason,j.testMode,saved->home,saved->address,saved->landmark,callback_number(*saved),where,j.event,saved->addressFields.postalCode);
    strlcpy(j.stationMessage,message.c_str(),sizeof j.stationMessage);
    return enqueue(j);
}

std::string settings_json() {
    Options o = get_options();
    std::unique_ptr<settings::Config> c(new settings::Config(settings::get()));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "smsEnabled", o.enabled);
    cJSON_AddBoolToObject(j, "hasSmsApiKey", o.apiKey[0]);
    cJSON_AddStringToObject(j, "smsDeviceId", o.deviceId);
    {
        std::lock_guard<std::mutex> g(lock);
        cJSON_AddBoolToObject(j, "stationSms", stationOptions.enabled);
    }
    auto station = station_number(*c);
    cJSON_AddStringToObject(j, "stationNumber", station.c_str());
    cJSON *ok = cJSON_AddArrayToObject(j, "smsRecipients");
    cJSON *bad = cJSON_AddArrayToObject(j, "smsInvalid");
    for (auto &v : c->contacts) {
        if (!v.phone[0])
            continue;
        auto n = sms_policy::normalise_phone(v.phone);
        cJSON_AddItemToArray(n.empty() ? bad : ok, cJSON_CreateString(n.empty() ? v.name : n.c_str()));
    }
    char *s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(j);
    return out;
}

bool parse_save(const char *json, std::string &error) {
    Options o = get_options();
    StationOptions so;
    {
        std::lock_guard<std::mutex> g(lock);
        so = stationOptions;
    }
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        error = "Expected JSON object";
        return false;
    }
    bool ok = true;
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "smsEnabled")) {
        if (cJSON_IsBool(v))
            o.enabled = cJSON_IsTrue(v);
        else
            ok = false;
    }
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "smsApiKey")) {
        // Blank keeps the saved key, like the Telegram token field.
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof o.apiKey || !token_ok(v->valuestring))
            ok = false;
        else if (v->valuestring[0])
            strlcpy(o.apiKey, v->valuestring, sizeof o.apiKey);
    }
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "stationSms")) {
        if (cJSON_IsBool(v))
            so.enabled = cJSON_IsTrue(v);
        else
            ok = false;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "smsClearApiKey")))
        o.apiKey[0] = 0;
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "smsDeviceId")) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof o.deviceId || !token_ok(v->valuestring))
            ok = false;
        else
            strlcpy(o.deviceId, v->valuestring, sizeof o.deviceId);
    }
    cJSON_Delete(j);
    if (!ok) {
        error = "Invalid SMS field: the API key and device ID may only contain letters, digits, - _ .";
        return false;
    }
    if (o.enabled && !o.apiKey[0]) {
        error = "Enter the textbee API key to turn SMS alerts on";
        return false;
    }
    if (so.enabled && !o.enabled) {
        error = "Turn SMS alerts on (with the textbee API key) to SMS the fire station";
        return false;
    }
    if (so.enabled) {
        std::unique_ptr<settings::Config> c(new settings::Config(settings::get()));
        if (station_number(*c).empty()) {
            error = "Fire-station SMS needs a full mobile number in step 02 (e.g. 98765 43210). Short codes "
                    "like 101 or 112 cannot receive SMS";
            return false;
        }
    }
    std::lock_guard<std::mutex> g(lock);
    if (!persist(o) || !persist(o, "station", sizeof so, &so)) {
        error = "NVS write failed";
        return false;
    }
    options = o;
    stationOptions = so;
    return true;
}

Health health() {
    Health h;
    {
        std::lock_guard<std::mutex> g(lock);
        h.enabled = options.enabled;
        h.configured = options.apiKey[0];
        h.station = options.enabled && options.apiKey[0] && stationOptions.enabled;
    }
    h.stationSent = stationSent;
    h.sent = sent;
    h.failed = failed;
    h.skipped = skipped;
    h.lastStatus = lastStatus;
    return h;
}
} // namespace sms
