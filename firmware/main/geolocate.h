#pragma once
// Wi-Fi geolocation for the hybrid location feature (no GPS hardware). The ESP32 scans nearby
// Wi-Fi access points and asks the Google Geolocation API where they are. The result is cached in
// NVS and refreshed once a day while SAFE; alerts only read the cache. See location_policy.h for
// how the fixed (installer) location and the Wi-Fi fix are combined.
#include "location_policy.h"
#include <stdint.h>
#include <string>

namespace geolocate {
struct Health {
    bool enabled = false, configured = false, busy = false;
    location_policy::WifiFix fix;
    uint32_t lookups = 0, failures = 0;
    int lastStatus = 0;  // last HTTP status from Google (0 = none yet / network error)
    int lastAps = 0;     // access points sent in the last lookup
    char lastError[96] = {};
};
void init();  // load options + cached fix from NVS; call after nvs_flash_init()
void start(); // start the background lookup task
void set_alarm(bool active);  // from alerts::observe(): no Wi-Fi scans while an alarm is active
bool detect_now();            // setup page "Detect now" (30 s cooldown)
location_policy::Resolved current(); // best location right now: fixed first, Wi-Fi fallback
std::string settings_json(); // never contains the API key
bool parse_save(const char *json, std::string &error);
Health health();
} // namespace geolocate
