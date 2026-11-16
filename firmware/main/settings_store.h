#pragma once
#include "nvs.h"
#include <initializer_list>
namespace settings_store {
constexpr const char *PARTITION = "fg_config";
inline esp_err_t open_write(const char *ns, nvs_handle_t *handle) {
    return nvs_open_from_partition(PARTITION, ns, NVS_READWRITE, handle);
}
// Prefer the new partition, with per-key read-only fallback to legacy settings.
// An invalid new value is not replaced by an older value; only absent keys fall back.
inline esp_err_t read_blob(const char *ns, const char *key, void *data, size_t *size) {
    const size_t capacity = *size;
    esp_err_t error = ESP_ERR_NVS_NOT_FOUND;
    for (const char *partition : {PARTITION, "nvs"}) {
        nvs_handle_t handle;
        error = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
        if (error != ESP_OK) continue;
        *size = capacity;
        error = nvs_get_blob(handle, key, data, size);
        nvs_close(handle);
        if (error != ESP_ERR_NVS_NOT_FOUND) return error;
    }
    return error;
}
} // namespace settings_store
