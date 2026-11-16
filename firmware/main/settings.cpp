#include "settings.h"
#include "cJSON.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <math.h>
#include <memory>
#include <string.h>
namespace settings {
namespace {
Config config;
SemaphoreHandle_t lock;
constexpr const char *CONFIG_PARTITION = "fg_config";
esp_err_t lastPersistError = ESP_OK;
bool persist(const Config &c) {
    nvs_handle_t h;
    lastPersistError = nvs_open_from_partition(CONFIG_PARTITION, "fg-settings", NVS_READWRITE, &h);
    if (lastPersistError == ESP_OK) {
        lastPersistError = nvs_set_blob(h, "config", &c, sizeof c);
        if (lastPersistError == ESP_OK) lastPersistError = nvs_commit(h);
        nvs_close(h);
    }
    if (lastPersistError != ESP_OK)
        ESP_LOGW("settings", "Settings storage failed: %s", esp_err_to_name(lastPersistError));
    return lastPersistError == ESP_OK;
}
bool load(const char *partition, Config &c) {
    nvs_handle_t h;
    if (nvs_open_from_partition(partition, "fg-settings", NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t n = sizeof c;
    bool valid = nvs_get_blob(h, "config", &c, &n) == ESP_OK && n == sizeof c && c.version == 4;
    nvs_close(h);
    return valid;
}
bool text(cJSON *j, const char *key, char *dst, size_t n, bool secret = false) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!v)
        return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= n)
        return false;
    if (secret && !v->valuestring[0])
        return true;
    for (const char *p = v->valuestring; *p; p++)
        if ((unsigned char)*p < 32)
            return false;
    strlcpy(dst, v->valuestring, n);
    return true;
}
bool address(cJSON *j, Address &a) {
    if (!j)
        return true;
    if (!cJSON_IsObject(j))
        return false;
    return text(j, "unit", a.unit, sizeof a.unit) && text(j, "building", a.building, sizeof a.building) &&
           text(j, "street", a.street, sizeof a.street) && text(j, "area", a.area, sizeof a.area) &&
           text(j, "city", a.city, sizeof a.city) && text(j, "state", a.state, sizeof a.state) &&
           text(j, "postalCode", a.postalCode, sizeof a.postalCode) &&
           text(j, "country", a.country, sizeof a.country);
}
cJSON *address_json(const Address &a) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "unit", a.unit);
    cJSON_AddStringToObject(j, "building", a.building);
    cJSON_AddStringToObject(j, "street", a.street);
    cJSON_AddStringToObject(j, "area", a.area);
    cJSON_AddStringToObject(j, "city", a.city);
    cJSON_AddStringToObject(j, "state", a.state);
    cJSON_AddStringToObject(j, "postalCode", a.postalCode);
    cJSON_AddStringToObject(j, "country", a.country);
    return j;
}
} // namespace
std::string format_address(const Address &a) {
    std::string out;
    for (const char *part : {a.unit, a.building, a.street, a.area, a.city, a.state, a.postalCode, a.country})
        if (part[0]) {
            if (!out.empty())
                out += ", ";
            out += part;
        }
    return out;
}
void init() {
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
    // Never erase existing settings to recover capacity. The new 64 KiB partition
    // occupies previously unassigned flash; legacy data remains available for migration.
    esp_err_t initialized = nvs_flash_init_partition(CONFIG_PARTITION);
    if (initialized != ESP_OK)
        ESP_LOGW("settings", "Settings partition init failed: %s", esp_err_to_name(initialized));
    std::unique_ptr<Config> stored(new Config);
    bool current = initialized == ESP_OK && load(CONFIG_PARTITION, *stored);
    bool legacy = !current && load("nvs", *stored);
    if (current || legacy) config = *stored;
    bool changed = legacy;
    if (!config.pin[0]) {
        snprintf(config.pin, sizeof config.pin, "%08lu", (unsigned long)(esp_random() % 100000000));
        changed = true;
    }
    if (!config.emergencyPin[0]) {
        snprintf(config.emergencyPin, sizeof config.emergencyPin, "%08lu",
                 (unsigned long)(esp_random() % 100000000));
        changed = true;
    }
    if (!config.setupPassword[0]) {
        snprintf(config.setupPassword, sizeof config.setupPassword, "fg%08lx", (unsigned long)esp_random());
        changed = true;
    }
    if (changed) {
        bool saved = persist(config);
        if (legacy && saved) ESP_LOGI("settings", "Legacy settings copied to fg_config; original retained");
    }
}
CloudOptions cloud_options() {
    xSemaphoreTake(lock, portMAX_DELAY);
    CloudOptions o{};
    o.enabled = config.cloudEnabled;
    strlcpy(o.url, config.cloudUrl, sizeof o.url);
    strlcpy(o.token, config.cloudDeviceToken, sizeof o.token);
    xSemaphoreGive(lock);
    return o;
}
AlertOptions alert_options() {
    xSemaphoreTake(lock, portMAX_DELAY);
    AlertOptions o{config.emergencyEnabled,  config.emergencyTest,  config.emergencyChat != 0,
                   config.unattendedSeconds, config.fireConfidence, config.emergencyChat};
    xSemaphoreGive(lock);
    return o;
}
int registered_count() {
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = 0;
    for (auto &c : config.contacts)
        if (c.chat)
            n++;
    xSemaphoreGive(lock);
    return n;
}
bool has_wifi() {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool v = config.ssid[0];
    xSemaphoreGive(lock);
    return v;
}
Config get() {
    xSemaphoreTake(lock, portMAX_DELAY);
    Config c = config;
    xSemaphoreGive(lock);
    return c;
}
bool save(const Config &c) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = persist(c);
    if (ok)
        config = c;
    xSemaphoreGive(lock);
    return ok;
}
bool join(int64_t chat, const char *name) {
    if (chat <= 0)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    std::unique_ptr<Config> saved(new Config(config));
    Config &c=*saved;
    int slot = -1;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        if (c.contacts[i].chat == chat) {
            xSemaphoreGive(lock);
            return true;
        }
        if (!c.contacts[i].chat && slot < 0)
            slot = i;
    }
    bool ok = false;
    if (slot >= 0) {
        auto &v = c.contacts[slot];
        v.chat = chat;
        if (!v.name[0])
            strlcpy(v.name, name, sizeof v.name);
        ok = persist(c);
        if (ok)
            config = c;
    }
    xSemaphoreGive(lock);
    return ok;
}
bool join_emergency(int64_t chat, const char *name) {
    if (chat <= 0)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    std::unique_ptr<Config> saved(new Config(config));
    Config &c=*saved;
    c.emergencyChat = chat;
    strlcpy(c.emergencyName, name, sizeof c.emergencyName);
    bool ok = persist(c);
    if (ok)
        config = c;
    xSemaphoreGive(lock);
    return ok;
}
std::string public_json(bool install) {
    std::unique_ptr<Config> saved(new Config(get()));
    Config &c=*saved;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "home", c.home);
    cJSON_AddStringToObject(j, "address", c.address);
    cJSON_AddStringToObject(j, "landmark", c.landmark);
    cJSON_AddStringToObject(j, "station", c.station);
    cJSON_AddStringToObject(j, "stationPhone", c.stationPhone);
    cJSON_AddStringToObject(j, "ambulancePhone", c.ambulancePhone);
    cJSON_AddStringToObject(j, "emergencyPhone", c.emergencyPhone);
    cJSON_AddStringToObject(j, "emergencyName", c.emergencyName);
    cJSON_AddBoolToObject(j, "emergencyJoined", c.emergencyChat != 0);
    cJSON_AddBoolToObject(j, "emergencyEnabled", c.emergencyEnabled);
    cJSON_AddBoolToObject(j, "emergencyTest", c.emergencyTest);
    cJSON_AddNumberToObject(j, "unattendedSeconds", c.unattendedSeconds);
    cJSON_AddNumberToObject(j, "fireConfidence", c.fireConfidence);
    cJSON_AddItemToObject(j, "addressFields", address_json(c.addressFields));
    if (install) {
        cJSON_AddStringToObject(j, "ssid", c.ssid);
        cJSON_AddBoolToObject(j, "cloudEnabled", c.cloudEnabled);
        cJSON_AddStringToObject(j, "cloudUrl", c.cloudUrl);
        cJSON_AddBoolToObject(j, "hasCloudToken", c.cloudDeviceToken[0]);
        cJSON_AddBoolToObject(j, "hasToken", c.token[0]);
        cJSON_AddBoolToObject(j, "hasPassword", c.password[0]);
        cJSON_AddStringToObject(j, "pin", c.pin);
        cJSON_AddStringToObject(j, "emergencyPin", c.emergencyPin);
    }
    cJSON_AddBoolToObject(j, "hasLocation", c.hasLocation);
    cJSON_AddNumberToObject(j, "latitude", c.latitude);
    cJSON_AddNumberToObject(j, "longitude", c.longitude);
    cJSON *a = cJSON_AddArrayToObject(j, "contacts");
    for (auto &v : c.contacts) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "name", v.name);
        cJSON_AddStringToObject(r, "phone", v.phone);
        cJSON_AddStringToObject(r, "address", v.address);
        cJSON_AddItemToObject(r, "addressFields", address_json(v.addressFields));
        cJSON_AddBoolToObject(r, "joined", v.chat != 0);
        cJSON_AddItemToArray(a, r);
    }
    char *s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(j);
    return out;
}
bool parse_save(const char *json, std::string &error, bool *networkChanged) {
    if (networkChanged) *networkChanged = false;
    std::unique_ptr<Config> saved(new Config(get()));
    Config &c=*saved;
    bool remove[MAX_CONTACTS] = {};
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        error = "Expected JSON object";
        return false;
    }
    bool ok = text(j, "ssid", c.ssid, sizeof c.ssid) &&
              text(j, "password", c.password, sizeof c.password, true) &&
              text(j, "token", c.token, sizeof c.token, true) && text(j, "pin", c.pin, sizeof c.pin, true) &&
              text(j, "home", c.home, sizeof c.home) && text(j, "address", c.address, sizeof c.address) &&
              text(j, "landmark", c.landmark, sizeof c.landmark) &&
              text(j, "station", c.station, sizeof c.station) &&
              text(j, "cloudUrl", c.cloudUrl, sizeof c.cloudUrl) &&
              text(j, "cloudDeviceToken", c.cloudDeviceToken, sizeof c.cloudDeviceToken, true) &&
              text(j, "stationPhone", c.stationPhone, sizeof c.stationPhone) &&
              text(j, "ambulancePhone", c.ambulancePhone, sizeof c.ambulancePhone) &&
              text(j, "emergencyPhone", c.emergencyPhone, sizeof c.emergencyPhone) &&
              text(j, "emergencyPin", c.emergencyPin, sizeof c.emergencyPin, true);
    if (auto *v = cJSON_GetObjectItemCaseSensitive(j, "cloudEnabled")) {
        if (!cJSON_IsBool(v))
            ok = false;
        else
            c.cloudEnabled = cJSON_IsTrue(v);
    }
    if (c.cloudEnabled && (strncmp(c.cloudUrl, "https://", 8) || !c.cloudDeviceToken[0]))
        ok = false;
    cJSON *homeFields = cJSON_GetObjectItemCaseSensitive(j, "addressFields");
    ok = ok && address(homeFields, c.addressFields);
    if (homeFields) {
        auto formatted = format_address(c.addressFields);
        if (formatted.size() >= sizeof c.address)
            ok = false;
        else
            strlcpy(c.address, formatted.c_str(), sizeof c.address);
    }
    for (auto pair : {std::pair<const char *, bool *>("emergencyEnabled", &c.emergencyEnabled),
                      {"emergencyTest", &c.emergencyTest}}) {
        if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, pair.first)) {
            if (!cJSON_IsBool(v))
                ok = false;
            else
                *pair.second = cJSON_IsTrue(v);
        }
    }
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "unattendedSeconds")) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 30 || v->valuedouble > 900 ||
            floor(v->valuedouble) != v->valuedouble)
            ok = false;
        else
            c.unattendedSeconds = uint32_t(v->valuedouble);
    }
    if (cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "fireConfidence")) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 0.7 || v->valuedouble > 0.99)
            ok = false;
        else
            c.fireConfidence = v->valuedouble;
    }
    if (strlen(c.emergencyPin) < 6)
        ok = false;
    cJSON *lat = cJSON_GetObjectItemCaseSensitive(j, "latitude"),
          *lon = cJSON_GetObjectItemCaseSensitive(j, "longitude");
    if (lat || lon) {
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon) && isfinite(lat->valuedouble) &&
            isfinite(lon->valuedouble) && fabs(lat->valuedouble) <= 90 && fabs(lon->valuedouble) <= 180) {
            c.latitude = lat->valuedouble;
            c.longitude = lon->valuedouble;
        } else
            ok = false;
    }
    if (cJSON *loc = cJSON_GetObjectItemCaseSensitive(j, "hasLocation")) {
        if (!cJSON_IsBool(loc))
            ok = false;
        else
            c.hasLocation = cJSON_IsTrue(loc);
    }
    cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "contacts");
    if (a) {
        if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > MAX_CONTACTS)
            ok = false;
        else
            for (int i = 0; i < cJSON_GetArraySize(a); i++) {
                cJSON *r = cJSON_GetArrayItem(a, i);
                ok = ok && cJSON_IsObject(r) &&
                     text(r, "name", c.contacts[i].name, sizeof c.contacts[i].name) &&
                     text(r, "phone", c.contacts[i].phone, sizeof c.contacts[i].phone) &&
                     text(r, "address", c.contacts[i].address, sizeof c.contacts[i].address) &&
                     address(cJSON_GetObjectItemCaseSensitive(r, "addressFields"),
                             c.contacts[i].addressFields);
                if (cJSON_GetObjectItemCaseSensitive(r, "addressFields")) {
                    auto formatted = format_address(c.contacts[i].addressFields);
                    if (formatted.size() >= sizeof c.contacts[i].address)
                        ok = false;
                    else
                        strlcpy(c.contacts[i].address, formatted.c_str(), sizeof c.contacts[i].address);
                }

                remove[i] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "remove"));
                if (remove[i])
                    c.contacts[i].chat = 0;
            }
    }
    // Bot and Wi-Fi credentials are never reflected in GET responses. Token is restricted to Telegram's token
    // alphabet.
    if (c.token[0])
        for (const char *p = c.token; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  *p == ':' || *p == '_' || *p == '-'))
                ok = false;
    if (strlen(c.pin) < 6 || !c.ssid[0] || !c.home[0] || (c.password[0] && strlen(c.password) < 8))
        ok = false;
    cJSON_Delete(j);
    if (!ok) {
        error = "Invalid field: check lengths, Wi-Fi password (8+), join PIN (6+), home and coordinates";
        return false;
    }
    // Keep any Telegram registration that arrived while the form was being parsed.
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!strcmp(c.token, config.token))
        for (int i = 0; i < MAX_CONTACTS; i++) {
            if (!remove[i])
                c.contacts[i].chat = config.contacts[i].chat;
        }
    if (!strcmp(c.token, config.token)) {
        if (cJSON *root = cJSON_Parse(json)) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "removeEmergency")))
                c.emergencyChat = 0;
            else {
                c.emergencyChat = config.emergencyChat;
                strlcpy(c.emergencyName, config.emergencyName, sizeof c.emergencyName);
            }
            cJSON_Delete(root);
        }
    } else {
        // Compare under the lock without creating a full Config temporary on
        // the HTTP stack. A changed bot invalidates every old registration.
        for (auto &v : c.contacts)
            v.chat = 0;
        c.emergencyChat = 0;
    }
    const bool changed = strcmp(c.ssid, config.ssid) || strcmp(c.password, config.password);
    bool stored = persist(c);
    const esp_err_t writeError = lastPersistError;
    if (stored) {
        config = c;
        if (networkChanged) *networkChanged = changed;
    }
    xSemaphoreGive(lock);
    if (!stored) {
        error = std::string("NVS write failed (") + esp_err_to_name(writeError) + ")";
        return false;
    }
    return true;
}
} // namespace settings
