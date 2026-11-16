// Camera with power saving: it is only switched on for the moment a photo is taken.
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <vector>

namespace camera {
constexpr int WIDTH = 320;   // QVGA
constexpr int HEIGHT = 240;
constexpr int BYTES = WIDTH * HEIGHT * 2;  // RGB565

// Puts the camera's power-down pin (if wired) into the "off" state at boot.
void init_power_pin();

// Powers the camera up, lets exposure settle, takes one RGB565 photo into `dst`
// (big-endian RGB565, BYTES long) and powers the camera straight back down.
esp_err_t take_photo(uint8_t *dst, std::vector<uint8_t> *jpeg = nullptr);
const char *last_error_text();
}  // namespace camera
