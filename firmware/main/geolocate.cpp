#include "geolocate.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings_store.h"
#include "settings.h"
#include "wifi_sync.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string.h>
#include <time.h>

namespace geolocate {
namespace {
constexpr const char *TAG = "geo";
constexpr const char *URL = "https://www.googleapis.com/geolocation/v1/geolocate?key=";
constexpr int MAX_APS = 20;

// Own NVS namespace, so the main settings blob (and data already saved on devices) is unchanged.
struct Options {
    uint32_t version = 1;
    bool enabled = false;
    char apiKey[96] = {};
};
struct StoredFix {
    uint32_t version = 1;
    location_policy::WifiFix fix;
};

Options options;
location_policy::WifiFix cached;
uint32_t generation=0;
std::mutex lock;
std::atomic<bool> alarm{false}, requested{false}, busy{false};
std::atomic<uint32_t> lookups{0}, failures{0};
std::atomic<int> lastStatus{0}, lastAps{0};
char lastError[96] = {};

uint32_t now_ms() { return uint32_t(pdTICKS_TO_MS(xTaskGetTickCount())); }

void set_error(const char *e) {
    std::lock_guard<std::mutex> g(lock);
    strlcpy(lastError, e, sizeof lastError);
}

bool store(const char *key, const void *v, size_t n) {
    nvs_handle_t h;
    if (settings_store::open_write("fg-geo", &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_set_blob(h, key, v, n);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// Google API keys are [A-Za-z0-9_-]; anything else could break the URL.
bool key_ok(const char *s) {
    for (const char *p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' ||
              *p == '_'))
            return false;
    return true;
}

struct Reply {
    std::string body;
    bool overflow = false;
};
esp_err_t on_http(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        auto *r = (Reply *)e->user_data;
        if (r->body.size() + e->data_len > 4096) {
            r->overflow = true;
            return ESP_FAIL;
        }
        r->body.append((const char *)e->data, e->data_len);
    }
    return ESP_OK;
}

// Builds the request body from a Wi-Fi scan. Returns "" when fewer than 2 usable APs are visible.
std::string scan_body(int &count) {
    count = 0;
    wifi_scan_config_t sc = {};
    sc.show_hidden = true;
    sc.scan_type=WIFI_SCAN_TYPE_PASSIVE; sc.scan_time.passive=40;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        set_error("Wi-Fi scan could not start (Wi-Fi busy); will retry");
        return "";
    }
    if(alarm.load()) {set_error("Location scan paused for alert");return "";}
    uint8_t seen[MAX_APS][6]={};
    uint16_t n = MAX_APS;
    std::unique_ptr<wifi_ap_record_t[]> aps(new wifi_ap_record_t[MAX_APS]);
    if (esp_wifi_scan_get_ap_records(&n, aps.get()) != ESP_OK)
        n = 0;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "considerIp", false); // IP location is city-level: worse than nothing here
    cJSON *list = cJSON_AddArrayToObject(j, "wifiAccessPoints");
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        size_t len = strnlen(ssid, sizeof aps[i].ssid);
        if (len >= 6 && !strncmp(ssid + len - 6, "_nomap", 6))
            continue; // the owner opted this network out of location services
        char mac[18];
        const uint8_t *b = aps[i].bssid;
        if((b[0]&3) || (!b[0]&&!b[1]&&!b[2]&&!b[3]&&!b[4]&&!b[5]) || (!b[0]&&!b[1]&&b[2]==0x5e))continue;
        bool duplicate=false;
        for(int k=0;k<count;++k) if(!memcmp(seen[k],b,6))duplicate=true;
        if(duplicate || aps[i].rssi>-10 || aps[i].rssi < -127)continue;
        memcpy(seen[count],b,6);
        snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "macAddress", mac);
        cJSON_AddNumberToObject(ap, "signalStrength", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", aps[i].primary);
        cJSON_AddItemToArray(list, ap);
        count++;
    }
    std::string out;
    if (count >= 2) {
        char *s = cJSON_PrintUnformatted(j);
        out = s ? s : "";
        cJSON_free(s);
    } else
        set_error("Fewer than 2 Wi-Fi networks visible; Wi-Fi location needs at least 2");
    cJSON_Delete(j);
    return out;
}

// One lookup. Returns true and fills `fix` on success.
bool lookup(const Options &o, location_policy::WifiFix &fix) {
    int count = 0;
    std::string body = scan_body(count);
    lastAps = count;
    if (body.empty() || alarm.load())
        return false;
    std::string url = std::string(URL) + o.apiKey;
    Reply reply;
    esp_http_client_config_t c = {};
    c.url = url.c_str();
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 10000;
    c.buffer_size = 2048;
    c.disable_auto_redirect = true;
    c.event_handler = on_http;
    c.user_data = &reply;
    auto client = esp_http_client_init(&c);
    if (!client)
        return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.data(), body.size());
    int status = esp_http_client_perform(client) == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    lastStatus = status;
    cJSON *j = reply.overflow ? nullptr : cJSON_Parse(reply.body.c_str());
    bool ok = false;
    if (status == 200 && j) {
        cJSON *loc = cJSON_GetObjectItemCaseSensitive(j, "location");
        cJSON *lat = cJSON_GetObjectItemCaseSensitive(loc, "lat"), *lng = cJSON_GetObjectItemCaseSensitive(loc, "lng");
        cJSON *acc = cJSON_GetObjectItemCaseSensitive(j, "accuracy");
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lng) && cJSON_IsNumber(acc) &&
            location_policy::usable(lat->valuedouble, lng->valuedouble) && isfinite(acc->valuedouble) && acc->valuedouble > 0 && acc->valuedouble <= location_policy::MAX_WIFI_ACCURACY_M) {
            fix.valid = true;
            fix.lat = lat->valuedouble;
            fix.lng = lng->valuedouble;
            fix.accuracyM = float(acc->valuedouble);
            fix.when = time(nullptr);
            ok = true;
            set_error("");
        } else
            set_error("Unexpected reply from the Geolocation API");
    } else {
        // Google error replies: {"error":{"code":403,"message":"...","status":"PERMISSION_DENIED"}}
        char e[96];
        snprintf(e, sizeof e, "%s", status == 404   ? "Google could not place these Wi-Fi networks (404)"
                                    : status == 400 ? "API key invalid or Geolocation API not enabled (400)"
                                    : status == 403 ? "API key refused: enable billing/the Geolocation API (403)"
                                    : status == 0   ? "No internet / TLS error reaching Google"
                                    : "Geolocation API error; check the provider dashboard");
        set_error(e);
        ESP_LOGW(TAG, "Wi-Fi location failed: HTTP %d (%s)", status, e);
    }
    cJSON_Delete(j);
    return ok;
}

void task(void *) {
    bool everTried = false;
    uint32_t last = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        Options o;
        location_policy::WifiFix have;
        uint32_t version=0;
        {
            std::lock_guard<std::mutex> g(lock);
            o = options;
            have = cached; version=generation;
        }
        bool manual = requested.load();
        bool due = location_policy::should_refresh(o.enabled && o.apiKey[0], alarm.load(), location_policy::fresh(have,time(nullptr)), everTried,
                                                   now_ms() - last);
        // TLS needs Wi-Fi and the real time (SNTP).
        if (!(due || (manual && o.enabled && o.apiKey[0] && !alarm.load())) || !wifi_sync::connected() ||
            time(nullptr) < 1700000000)
            continue;
        requested = false;
        everTried = true;
        last = now_ms();
        busy = true;
        lookups++;
        location_policy::WifiFix fix;
        if (lookup(o, fix)) {
            std::lock_guard<std::mutex> g(lock);
            // Disabling/forgetting/changing key while a lookup runs invalidates its result.
            if(version==generation && options.enabled && !alarm.load()) {
                StoredFix s; s.fix=fix;
                if(store("fix",&s,sizeof s)) cached=fix;
                else {failures++;strlcpy(lastError,"Could not persist location; retry detection",sizeof lastError);}
                ESP_LOGI(TAG,"Wi-Fi location cached; accuracy about %.0f m",double(fix.accuracyM));
            }
        } else
            failures++; // the previous cached fix (if any) stays in use
        busy = false;
    }
}
} // namespace

void init() {
    Options o;
    size_t n = sizeof o;
    if (settings_store::read_blob("fg-geo", "opts", &o, &n) == ESP_OK && n == sizeof o && o.version == 1) {
        o.apiKey[sizeof o.apiKey - 1] = 0;
        std::lock_guard<std::mutex> g(lock);
        options = o;
    }
    StoredFix s;
    n = sizeof s;
    if (settings_store::read_blob("fg-geo", "fix", &s, &n) == ESP_OK && n == sizeof s && s.version == 1 && s.fix.valid) {
        std::lock_guard<std::mutex> g(lock);
        cached = s.fix;
    }
}

void start() { configASSERT(xTaskCreate(task, "geolocate", 8192, nullptr, 1, nullptr) == pdPASS); }

void set_alarm(bool active) {
    bool was=alarm.exchange(active);
    if(active&&!was&&busy.load())esp_wifi_scan_stop();
}

bool detect_now() {
    {
        std::lock_guard<std::mutex> g(lock);
        if (!options.enabled || !options.apiKey[0])
            return false;
    }
    static std::atomic<uint32_t> lastAsk{0};
    uint32_t now = now_ms(), prev = lastAsk.load();
    if (alarm || busy || (prev && now - prev < 30000) || !lastAsk.compare_exchange_strong(prev, now))
        return false;
    requested = true;
    return true;
}

location_policy::Resolved current() {
    location_policy::WifiFix w;
    {
        std::lock_guard<std::mutex> g(lock);
        w = options.enabled && options.apiKey[0] ? cached : location_policy::WifiFix{};
    }
    std::unique_ptr<settings::Config> c(new settings::Config(settings::get()));
    return location_policy::resolve(c->hasLocation, c->latitude, c->longitude, w, time(nullptr));
}

Health health() {
    Health h;
    std::lock_guard<std::mutex> g(lock);
    h.enabled = options.enabled;
    h.configured = options.apiKey[0];
    h.fix = options.enabled && options.apiKey[0] ? cached : location_policy::WifiFix{};
    strlcpy(h.lastError, lastError, sizeof h.lastError);
    h.busy = busy;
    h.lookups = lookups;
    h.failures = failures;
    h.lastStatus = lastStatus;
    h.lastAps = lastAps;
    return h;
}

std::string settings_json() {
    auto h = health();
    auto r = current();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "geoEnabled", h.enabled);
    cJSON_AddBoolToObject(j, "hasGeoApiKey", h.configured);
    cJSON_AddBoolToObject(j, "geoBusy", h.busy);
    cJSON_AddStringToObject(j, "geoError", h.lastError);
    cJSON_AddNumberToObject(j, "geoLastStatus", h.lastStatus);
    cJSON_AddNumberToObject(j, "geoNetworks", h.lastAps);
    if (h.fix.valid) {
        cJSON *f = cJSON_AddObjectToObject(j, "wifiFix");
        cJSON_AddNumberToObject(f, "latitude", h.fix.lat);
        cJSON_AddNumberToObject(f, "longitude", h.fix.lng);
        cJSON_AddNumberToObject(f, "accuracy", h.fix.accuracyM);
        int64_t age = time(nullptr) >= 1700000000 ? (time(nullptr) - h.fix.when) / 60 : -1;
        cJSON_AddNumberToObject(f, "ageMinutes", double(age));
    }
    cJSON *u = cJSON_AddObjectToObject(j, "inUse");
    cJSON_AddStringToObject(u, "source", location_policy::source_name(r.source));
    if (r.valid) {
        cJSON_AddNumberToObject(u, "latitude", r.lat);
        cJSON_AddNumberToObject(u, "longitude", r.lng);
        cJSON_AddNumberToObject(u, "accuracy", r.accuracyM);
        cJSON_AddStringToObject(u, "map", location_policy::maps_link(r.lat, r.lng).c_str());
    }
    cJSON_AddBoolToObject(u, "mismatch", r.mismatch);
    cJSON_AddNumberToObject(u, "mismatchMetres", double(int(r.mismatchM)));
    char *s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(j);
    return out;
}

bool parse_save(const char *json, std::string &error) {
    Options o;
    {
        std::lock_guard<std::mutex> g(lock);
        o = options;
    }
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        error = "Expected JSON object";
        return false;
    }
    bool ok = true;
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "geoEnabled")) {
        if (cJSON_IsBool(v))
            o.enabled = cJSON_IsTrue(v);
        else
            ok = false;
    }
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "geoApiKey")) {
        // Blank keeps the saved key, like the Telegram token and textbee key fields.
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof o.apiKey || !key_ok(v->valuestring))
            ok = false;
        else if (v->valuestring[0])
            strlcpy(o.apiKey, v->valuestring, sizeof o.apiKey);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "geoClearApiKey")))
        o.apiKey[0] = 0;
    bool forget = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "geoForgetFix"));
    cJSON_Delete(j);
    if (!ok) {
        error = "Invalid Wi-Fi location field: the API key may only contain letters, digits, - and _";
        return false;
    }
    if (o.enabled && !o.apiKey[0]) {
        error = "Enter a Google Geolocation API key to turn Wi-Fi location on";
        return false;
    }
    std::lock_guard<std::mutex> g(lock);
    if (!store("opts", &o, sizeof o)) {
        error = "NVS write failed";
        return false;
    }
    bool keyChanged = strcmp(o.apiKey, options.apiKey) != 0;
    options = o;
    ++generation;
    if (forget) {
        StoredFix s;
        if(!store("fix", &s, sizeof s)){ error="Could not persist forgotten location"; return false; }
        cached = {};
    }
    if (!o.enabled) requested=false;
    if (o.enabled && (keyChanged || !cached.valid))
        requested = true; // look up soon after the first save or a new key
    return true;
}
} // namespace geolocate
