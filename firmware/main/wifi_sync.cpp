#include <time.h>
#include "wifi_sync.h"
#include "alerts.h"
#include "cloud_link.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "geolocate.h"
#include "sensors.h"
#include "vision_service.h"
#include "settings.h"
#include "setup_access.h"
#include "network_apply.h"
#include "sms.h"
#include "storage.h"
#include "tar.h"
#include "web_page.h"
#include <atomic>
#include <ctype.h>
#include <dirent.h>
#include <lwip/sockets.h>
#include <mutex>
#include <string.h>
#include <string>
#include <memory>
#include <strings.h>
#include <sys/stat.h>
namespace {
std::atomic<bool> online{false}, ap_on{false}, setup_request{false};
NetworkApply network_apply;
std::mutex info_lock;
std::string ip = "connecting", ap_password;
char csrf[33];
httpd_handle_t server;
esp_netif_t *station_net, *setup_net;
void event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        online = false;
        esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t *)data;
        char b[32];
        snprintf(b, sizeof b, IPSTR, IP2STR(&e->ip_info.ip));
        {
            std::lock_guard<std::mutex> g(info_lock);
            ip = b;
        }
        online = true;
    }
}
bool on_ap(httpd_req_t *r) {
    if (!ap_on || !setup_net)
        return false;
    esp_netif_ip_info_t ap = {};
    sockaddr_storage local = {}, peer = {};
    socklen_t localLength = sizeof local, peerLength = sizeof peer;
    int fd = httpd_req_to_sockfd(r);
    if (esp_netif_get_ip_info(setup_net, &ap) != ESP_OK ||
        getsockname(fd, reinterpret_cast<sockaddr *>(&local), &localLength) != 0 ||
        getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peerLength) != 0)
        return false;
    return setup_access::allows(ap_on, reinterpret_cast<const sockaddr *>(&local), localLength,
                                reinterpret_cast<const sockaddr *>(&peer), peerLength,
                                ntohl(ap.ip.addr), ntohl(ap.netmask.addr));
}
bool mutation(httpd_req_t *r) {
    char token[40];
    return httpd_req_get_hdr_value_str(r, "X-FireGuard", token, sizeof token) == ESP_OK &&
           !strcmp(token, csrf);
}
esp_err_t json(httpd_req_t *r, cJSON *j) {
    char *b = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!b)
        return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    auto e = httpd_resp_send(r, b, HTTPD_RESP_USE_STRLEN);
    cJSON_free(b);
    return e;
}
std::string body(httpd_req_t *r) {
    if (r->content_len <= 0 || r->content_len > 8192)
        return {};
    std::string b(r->content_len, '\0');
    int n = 0;
    while (n < r->content_len) {
        int got = httpd_req_recv(r, &b[n], r->content_len - n);
        if (got <= 0)
            return {};
        n += got;
    }
    return b;
}
esp_err_t denied(httpd_req_t *r) {
    return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN,
                               "Use the FireGuard page; installation settings require the setup hotspot");
}
esp_err_t h_index(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, WEB_PAGE, HTTPD_RESP_USE_STRLEN);
}
esp_err_t h_status(httpd_req_t *r) {
    auto s = sensors::snapshot();
    auto h = alerts::health();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", STATE_NAME[s.state]);
    cJSON_AddStringToObject(j,"hardwareRevision","two-mq-ads1115-v2");
    cJSON_AddStringToObject(j,"softwareRevision","hybrid-location-v3.7-snapshot");
    cJSON_AddStringToObject(j,"sensorModel","two-sensor-relative-prototype");
    auto cameraHealth=vision_service::health();
    cJSON_AddBoolToObject(j,"cameraBusy",cameraHealth.busy);
    cJSON_AddBoolToObject(j,"cameraReady",cameraHealth.ready);
    cJSON_AddNumberToObject(j,"cameraImageAt",cameraHealth.imageAt);
    cJSON_AddNumberToObject(j,"cameraAttemptAt",cameraHealth.attemptedAt);
    cJSON_AddStringToObject(j,"cameraError",cameraHealth.reason);
    auto cloudHealth=cloud_link::health();
    cJSON_AddNumberToObject(j,"cloudLastStatus",cloudHealth.status);
    cJSON_AddNumberToObject(j,"cloudLastError",cloudHealth.error);
    cJSON_AddNumberToObject(j,"cloudLastSuccessMs",cloudHealth.lastSuccessMs);
    cJSON_AddNumberToObject(j,"telegramLastStatus",h.httpStatus);


    cJSON_AddBoolToObject(j,"coAvailable",false);
    cJSON_AddBoolToObject(j,"ads1115",s.adcOK);
    cJSON_AddBoolToObject(j,"mq2Valid",s.mq2OK);
    cJSON_AddBoolToObject(j,"mq135Valid",s.mq135OK);
    cJSON_AddNumberToObject(j,"mq2AdcMv",s.adcMv2);
    cJSON_AddNumberToObject(j,"mq135AdcMv",s.adcMv135);
    cJSON_AddNumberToObject(j,"mq2AoMv",s.adcMv2*2);
    cJSON_AddNumberToObject(j,"mq135AoMv",s.adcMv135*2);
    cJSON_AddNumberToObject(j,"mq2Deviation",s.dev[0]);
    cJSON_AddNumberToObject(j,"mq135Deviation",s.dev[2]);
    cJSON_AddNumberToObject(j, "frs", s.frs);
    cJSON_AddNumberToObject(j, "grs", s.grs);
    cJSON_AddNumberToObject(j, "crs", s.crs);
    cJSON_AddNumberToObject(j, "temperature", s.tC);
    cJSON_AddNumberToObject(j, "humidity", s.rh);
    cJSON_AddNumberToObject(j, "pressure", s.hPa);
    cJSON_AddBoolToObject(j, "cameraRequested", s.cameraRequested);
    cJSON_AddBoolToObject(j, "cameraValid", s.cameraValid);
    cJSON_AddNumberToObject(j, "visionAgeMs", s.visionAgeMs);
    cJSON_AddBoolToObject(j, "muted", s.muted);
    cJSON_AddBoolToObject(j, "sensorError", s.flags & 1);
    cJSON_AddBoolToObject(j, "warming", s.flags & 2);
    cJSON_AddBoolToObject(j, "calibrated", s.calibrated);
    cJSON_AddNumberToObject(j, "burninHours", s.burninSec / 3600.0);
    cJSON_AddNumberToObject(j, "label", s.label);
    cJSON_AddBoolToObject(j, "wifi", online);
    cJSON_AddBoolToObject(j, "telegram", h.internet);
    cJSON_AddNumberToObject(j, "registered", h.registered);
    cJSON_AddNumberToObject(j, "sent", h.sent);
    cJSON_AddNumberToObject(j, "failed", h.failed);
    cJSON_AddNumberToObject(j, "dropped", h.dropped);
    cJSON_AddNumberToObject(j, "event", h.event);
    cJSON_AddBoolToObject(j, "acknowledged", h.acknowledged);
    cJSON_AddBoolToObject(j, "forwarded", h.forwarded);
    auto m = sms::health();
    cJSON_AddBoolToObject(j, "smsEnabled", m.enabled && m.configured);
    cJSON_AddNumberToObject(j, "smsSent", m.sent);
    cJSON_AddNumberToObject(j, "smsFailed", m.failed);
    cJSON_AddNumberToObject(j, "smsSkipped", m.skipped);
    cJSON_AddNumberToObject(j, "smsLastStatus", m.lastStatus);
    cJSON_AddBoolToObject(j, "stationSms", m.station);
    cJSON_AddNumberToObject(j, "stationSmsSent", m.stationSent);
    {
        auto where = geolocate::current();
        auto g = geolocate::health();
        cJSON *loc = cJSON_AddObjectToObject(j, "location");
        cJSON_AddStringToObject(loc, "source", location_policy::source_name(where.source));
        cJSON_AddBoolToObject(loc,"valid",where.valid);
        if(where.valid){
            cJSON_AddNumberToObject(loc,"latitude",where.lat);
            cJSON_AddNumberToObject(loc,"longitude",where.lng);
        }
        cJSON_AddNumberToObject(loc, "accuracy", where.accuracyM);
        cJSON_AddBoolToObject(loc, "mismatch", where.mismatch);
        cJSON_AddBoolToObject(loc, "wifiEnabled", g.enabled && g.configured);
        cJSON_AddBoolToObject(loc, "wifiFix", g.fix.valid && location_policy::fresh(g.fix,time(nullptr)));
    }
    cJSON_AddNumberToObject(j, "samples", storage::count());
    cJSON_AddBoolToObject(j, "setup", on_ap(r));
    cJSON_AddStringToObject(j, "csrf", csrf);
    cJSON_AddItemToObject(j, "home", cJSON_Parse(settings::public_json(false).c_str()));
    return json(r, j);
}
esp_err_t h_settings(httpd_req_t *r) {
    if (!on_ap(r))
        return denied(r);
    if (r->method == HTTP_GET) {
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        auto b = settings::public_json(true);
        return httpd_resp_send(r, b.c_str(), b.size());
    }
    if (!mutation(r))
        return denied(r);
    auto b = body(r);
    std::string error;
    bool networkChanged = false;
    if (!settings::parse_save(b.c_str(), error, &networkChanged))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, error.c_str());
    char defer[4] = {};
    bool deferred = httpd_req_get_hdr_value_str(r, "X-FireGuard-Defer-Wifi", defer, sizeof defer) == ESP_OK && !strcmp(defer, "1");
    // Let the browser finish SMS/location writes before the AP radio changes channel.
    // Old clients still reconnect automatically; interrupted new clients have a 60s fallback.
    if (networkChanged) network_apply.schedule(millis(), deferred ? 60000 : 2000);
    httpd_resp_set_hdr(r, "X-FireGuard-Network-Pending", network_apply.pending() ? "1" : "0");
    return httpd_resp_sendstr(r, "Home settings saved.");
}
esp_err_t h_apply_network(httpd_req_t *r) {
    if (!on_ap(r) || !mutation(r)) return denied(r);
    bool pending = network_apply.pending();
    // Reply first, then allow time for the browser to show confirmation.
    if (pending) network_apply.schedule(millis(), 2000);
    return httpd_resp_sendstr(r, pending ?
        "Saved. Connecting to home Wi-Fi. If this page disconnects, rejoin FireGuard-Setup and reopen 192.168.4.1." :
        "Saved. Wi-Fi settings are unchanged.");
}
esp_err_t h_test(httpd_req_t *r) {
    if (!mutation(r))
        return denied(r);
    if (!alerts::test())
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Test queued recently or queue full; wait 30 seconds");
    return httpd_resp_sendstr(r, "Test queued for registered contacts");
}
esp_err_t h_capture(httpd_req_t *r) {
    if (!mutation(r)) return denied(r);
    if (!alerts::snapshot()) return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Wait 30 seconds between snapshots or retry after the queue clears");
    return httpd_resp_sendstr(r,"Snapshot requested. A fresh photo will appear here and be sent to registered Telegram residents; capture failure is reported explicitly.");
}
esp_err_t h_photo(httpd_req_t *r) {
    std::vector<uint8_t> photo;
    if (!vision_service::photo(photo,120000)) return httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"No recent camera photo; click Capture and send photo first");
    httpd_resp_set_type(r,"image/jpeg");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    return httpd_resp_send(r,reinterpret_cast<const char *>(photo.data()),photo.size());
}
esp_err_t h_ack(httpd_req_t *r) {
    if (!mutation(r))
        return denied(r);
    auto b = body(r);
    cJSON *j = cJSON_Parse(b.c_str());
    auto *v = cJSON_GetObjectItemCaseSensitive(j, "event");
    bool valid = cJSON_IsNumber(v) && v->valuedouble >= 1 && v->valuedouble <= UINT32_MAX &&
                 floor(v->valuedouble) == v->valuedouble;
    bool ok = valid && alerts::acknowledge(uint32_t(v->valuedouble));
    cJSON_Delete(j);
    if (!ok)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Event has ended; refresh the dashboard");
    return httpd_resp_sendstr(
        r, "Warning acknowledged. High-confidence fire forwarding still applies when enabled.");
}
esp_err_t h_emergency_test(httpd_req_t *r) {
    if (!on_ap(r) || !mutation(r))
        return denied(r);
    if (!alerts::test_emergency())
        return httpd_resp_send_err(
            r, HTTPD_400_BAD_REQUEST,
            "Register the Telegram emergency recipient, or wait 30 seconds between tests");
    return httpd_resp_sendstr(r, "Emergency recipient test queued. No phone call is made.");
}
// SMS (textbee) settings live only on the setup hotspot, like the Telegram bot token.
esp_err_t h_sms(httpd_req_t *r) {
    if (!on_ap(r))
        return denied(r);
    if (r->method == HTTP_GET) {
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        auto b = sms::settings_json();
        return httpd_resp_send(r, b.c_str(), b.size());
    }
    if (!mutation(r))
        return denied(r);
    auto b = body(r);
    std::string error;
    if (!sms::parse_save(b.c_str(), error))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, error.c_str());
    return httpd_resp_sendstr(r, "SMS settings saved.");
}
esp_err_t h_sms_test(httpd_req_t *r) {
    if (!on_ap(r) || !mutation(r))
        return denied(r);
    if (!sms::test())
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Turn SMS alerts on and save the API key first, or wait 30 seconds between tests");
    return httpd_resp_sendstr(r, "Test SMS queued for every resident phone number. Check the textbee app.");
}
// Hybrid location (Wi-Fi geolocation) settings live only on the setup hotspot, like the SMS key.
esp_err_t h_location(httpd_req_t *r) {
    if (!on_ap(r))
        return denied(r);
    if (r->method == HTTP_GET) {
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        auto b = geolocate::settings_json();
        return httpd_resp_send(r, b.c_str(), b.size());
    }
    if (!mutation(r))
        return denied(r);
    auto b = body(r);
    std::string error;
    if (!geolocate::parse_save(b.c_str(), error))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, error.c_str());
    return httpd_resp_sendstr(r, "Location settings saved.");
}
esp_err_t h_location_detect(httpd_req_t *r) {
    if (!on_ap(r) || !mutation(r))
        return denied(r);
    if (!wifi_sync::connected())
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Connect FireGuard to home Wi-Fi first (save step 01), then detect");
    if (!geolocate::detect_now())
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Turn Wi-Fi location on and save the API key first, or wait 30 seconds. "
                                   "Detection is paused during an alarm");
    return httpd_resp_sendstr(r, "Scanning nearby Wi-Fi networks. The result appears here in a few seconds.");
}
esp_err_t h_station_test(httpd_req_t *r) {
    if (!on_ap(r) || !mutation(r))
        return denied(r);
    if (!sms::test_station(geolocate::current()))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "Turn on SMS alerts and \"Also SMS the fire-station number\", save, and use a "
                                   "full phone number for the fire station; or wait 30 seconds between tests");
    return httpd_resp_sendstr(r, "Test SMS queued for the fire-station number. It is clearly marked TEST ONLY.");
}
bool safe_name(const char *n) {
    if (!*n || strlen(n) > 32 || strstr(n, "..")) {
        return false;
    }
    for (const char *p = n; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

esp_err_t send_file(httpd_req_t *req, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    char *buf = (char *)malloc(4096);
    size_t n;
    esp_err_t err = ESP_OK;
    while (buf && (n = fread(buf, 1, 4096, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    free(buf);
    httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

esp_err_t h_info(httpd_req_t *req) {

    char buf[96];
    snprintf(buf, sizeof(buf), "{\"count\":%d,\"storage\":\"%s\"}", storage::count(), storage::where());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_log(httpd_req_t *req) {
    storage::Guard guard;

    char path[64];
    snprintf(path, sizeof(path), "%s/LOG.CSV", storage::dir());
    httpd_resp_set_type(req, "text/csv");
    struct stat st;
    if (stat(path, &st) != 0) { // nothing saved yet
        return httpd_resp_send(req, "id,image,feedback,model_fire,model_smoke,boxes,took_ms\n",
                               HTTPD_RESP_USE_STRLEN);
    }
    return send_file(req, path);
}

esp_err_t h_file(httpd_req_t *req) {
    storage::Guard guard;

    char query[64], name[40];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !safe_name(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", storage::dir(), name);
    const char *dot = strrchr(name, '.');
    bool jpg = dot && (strcasecmp(dot, ".JPG") == 0);
    httpd_resp_set_type(req, jpg ? "image/jpeg" : "text/plain");
    return send_file(req, path);
}

// Streams every sample as one tar file: FIELD/F0001.JPG, FIELD/F0001.TXT, ..., FIELD/LOG.CSV
esp_err_t h_tar(httpd_req_t *req) {
    storage::Guard guard;

    httpd_resp_set_type(req, "application/x-tar");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"firecam_samples.tar\"");
    DIR *d = opendir(storage::dir());
    uint8_t *block = (uint8_t *)malloc(4096);
    if (!block) {
        return httpd_resp_send_500(req);
    }
    if (d) {
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.' || !safe_name(e->d_name)) {
                continue;
            }
            char path[300], name[300];
            snprintf(path, sizeof(path), "%s/%s", storage::dir(), e->d_name);
            snprintf(name, sizeof(name), "FIELD/%s", e->d_name);
            struct stat st;
            FILE *f = (stat(path, &st) == 0) ? fopen(path, "rb") : nullptr;
            if (!f) {
                continue;
            }
            tar::header(block, name, st.st_size, 0);
            httpd_resp_send_chunk(req, (const char *)block, tar::BLOCK);
            size_t n, sent = 0;
            while ((n = fread(block, 1, 4096, f)) > 0) {
                httpd_resp_send_chunk(req, (const char *)block, n);
                sent += n;
            }
            fclose(f);
            int pad = tar::padding(sent);
            if (pad) {
                memset(block, 0, pad);
                httpd_resp_send_chunk(req, (const char *)block, pad);
            }
        }
        closedir(d);
    }
    memset(block, 0, 2 * tar::BLOCK); // end of archive
    httpd_resp_send_chunk(req, (const char *)block, 2 * tar::BLOCK);
    free(block);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t h_clear(httpd_req_t *r) {
    if (!on_ap(r) || !mutation(r))
        return denied(r);
    char b[80];
    snprintf(b, sizeof b, "Deleted %d files", storage::clear());
    return httpd_resp_sendstr(r, b);
}
void start_server() {
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.max_uri_handlers = 24;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 3;
    c.send_wait_timeout = 3;
    ESP_ERROR_CHECK(httpd_start(&server, &c));
    const httpd_uri_t uris[] = {{"/", HTTP_GET, h_index, nullptr},
                                {"/api/status", HTTP_GET, h_status, nullptr},
                                {"/api/settings", HTTP_GET, h_settings, nullptr},
                                {"/api/settings", HTTP_POST, h_settings, nullptr},
                                {"/api/apply-network", HTTP_POST, h_apply_network, nullptr},
                                {"/api/test", HTTP_POST, h_test, nullptr},
                                {"/api/capture", HTTP_POST, h_capture, nullptr},
                                {"/api/photo", HTTP_GET, h_photo, nullptr},
                                {"/api/ack", HTTP_POST, h_ack, nullptr},
                                {"/api/emergency-test", HTTP_POST, h_emergency_test, nullptr},
                                {"/api/sms", HTTP_GET, h_sms, nullptr},
                                {"/api/sms", HTTP_POST, h_sms, nullptr},
                                {"/api/sms-test", HTTP_POST, h_sms_test, nullptr},
                                {"/api/location", HTTP_GET, h_location, nullptr},
                                {"/api/location", HTTP_POST, h_location, nullptr},
                                {"/api/location-detect", HTTP_POST, h_location_detect, nullptr},
                                {"/api/station-sms-test", HTTP_POST, h_station_test, nullptr},
                                {"/info", HTTP_GET, h_info, nullptr},
                                {"/log.csv", HTTP_GET, h_log, nullptr},
                                {"/file", HTTP_GET, h_file, nullptr},
                                {"/all.tar", HTTP_GET, h_tar, nullptr},
                                {"/clear", HTTP_POST, h_clear, nullptr}};
    for (auto &u : uris)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
}
void set_ap(bool enabled) {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    esp_wifi_set_mode(enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (enabled) {
        wifi_config_t c = {};
        strlcpy((char *)c.ap.ssid, "FireGuard-Setup", sizeof c.ap.ssid);
        c.ap.ssid_len = strlen((char *)c.ap.ssid);
        c.ap.max_connection = 2;
        c.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)c.ap.password, ap_password.c_str(), sizeof c.ap.password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &c));
    }
    ap_on = enabled;
}
void connect() {
    std::unique_ptr<settings::Config> saved(new settings::Config(settings::get()));
    auto &c=*saved;
    if (!c.ssid[0])
        return;
    online = false;
    esp_wifi_disconnect();
    wifi_config_t w = {};
    memcpy(w.sta.ssid, c.ssid, strlen(c.ssid));
    memcpy(w.sta.password, c.password, strlen(c.password));
    w.sta.threshold.authmode = c.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    esp_wifi_connect();
}
bool driverReady=false;
esp_err_t init_driver() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station_net = esp_netif_create_default_wifi_sta();
    setup_net = esp_netif_create_default_wifi_ap();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    // Application settings are saved in fg-settings. Do not allocate a second Wi-Fi NVS cache.
    c.nvs_enable=0;
    esp_err_t err=esp_wifi_init(&c);
    if(err!=ESP_OK)return err;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, nullptr));
    {
        std::unique_ptr<settings::Config> saved(new settings::Config(settings::get()));
        ap_password = saved->setupPassword;
    }
    // esp_wifi_init only allocates the driver. Start PHY/timers and the initial
    // hotspot synchronously, before application workers consume internal RAM.
    set_ap(!settings::has_wifi());
    err=esp_wifi_start();
    if(err!=ESP_OK)return err;
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // powered monitor: reduce local dashboard latency
    driverReady=true;
    return ESP_OK;
}
void network(void *) {
    bool installed = settings::has_wifi();
    uint32_t ap_started = millis();
    connect();
    start_server();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    for (;;) {
        if (setup_request.exchange(false)) {
            set_ap(true);
            ap_started = millis();
        }
        if (network_apply.take(millis())) {
            connect();
            installed = true;
            ap_started = millis();
        }
        // Keep recovery/setup available for 10 min after a physical B2 hold or a save.
        if (installed && ap_on && millis() - ap_started >= 600000UL)
            set_ap(false);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
} // namespace
namespace wifi_sync {
esp_err_t init() {return init_driver();}
void start() {
    if(!driverReady){ESP_LOGE("network","Wi-Fi unavailable; local sensing continues");return;}
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof bytes);
    for (int i = 0; i < 16; i++)
        snprintf(csrf + i * 2, 3, "%02x", bytes[i]);
    ESP_LOGI("setup", "Setup Wi-Fi: FireGuard-Setup | password: %s | http://192.168.4.1",
             ap_password.c_str());
    configASSERT(xTaskCreate(network, "network", 8192, nullptr, 3, nullptr) == pdPASS);
}
void setup() { setup_request = true; }
bool setup_active() { return ap_on; }
bool connected() { return online; }
std::string address() {
    std::lock_guard<std::mutex> g(info_lock);
    return ip;
}
std::string hotspot_password() { return ap_password; }
} // namespace wifi_sync
