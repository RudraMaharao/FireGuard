#pragma once
#include "nvs.h"
#include <stdint.h>
class Preferences {
    nvs_handle_t h = 0;

  public:
    ~Preferences() { end(); }
    bool begin(const char *ns, bool ro = false) {
        end();
        return nvs_open(ns, ro ? NVS_READONLY : NVS_READWRITE, &h) == ESP_OK;
    }
    void end() {
        if (h) {
            nvs_close(h);
            h = 0;
        }
    }
    float getFloat(const char *k, float d = 0) {
        float v;
        size_t n = sizeof(v);
        return h && nvs_get_blob(h, k, &v, &n) == ESP_OK && n == sizeof(v) ? v : d;
    }
    uint32_t getUInt(const char *k, uint32_t d = 0) {
        uint32_t v;
        return h && nvs_get_u32(h, k, &v) == ESP_OK ? v : d;
    }
    uint32_t getULong(const char *k, uint32_t d = 0) { return getUInt(k, d); }
    uint8_t getUChar(const char *k, uint8_t d = 0) {
        uint8_t v;
        return h && nvs_get_u8(h, k, &v) == ESP_OK ? v : d;
    }
    bool putFloat(const char *k, float v) {
        return h && nvs_set_blob(h, k, &v, sizeof(v)) == ESP_OK && nvs_commit(h) == ESP_OK;
    }
    bool putUInt(const char *k, uint32_t v) {
        return h && nvs_set_u32(h, k, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    }
    bool putULong(const char *k, uint32_t v) { return putUInt(k, v); }
    bool putUChar(const char *k, uint8_t v) {
        return h && nvs_set_u8(h, k, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    }
};
