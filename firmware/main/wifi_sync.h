#pragma once
#include "esp_err.h"
#include <string>
namespace wifi_sync {
esp_err_t init(); // initialize AND start the radio before application workers
void start();
void setup();
bool setup_active();
bool connected();
std::string address();
std::string hotspot_password();
} // namespace wifi_sync
