// Host test for firmware/main/geolocate.cpp with stand-in ESP-IDF headers (tests/support).
// Checks the Wi-Fi scan -> Google request, reply parsing, caching, alarms and the hybrid result.
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "geolocate.h"
#include "settings.h"
#include "nvs.h"
#include "wifi_sync.h"
#include <assert.h>
#include <iostream>
#include <string.h>

namespace wifi_sync {
bool connected() { return true; }
} // namespace wifi_sync

static void run_task(int delays) {
    fake_delay_budget = delays;
    try {
        fake_task_fn(nullptr);
    } catch (FakeStop &) {
    }
    fake_delay_budget = -1;
}

int main() {
    settings::init();
    nvs_full_partition="nvs"; // save all setup options into the larger partition
    geolocate::init();
    geolocate::start();
    assert(fake_task_fn);
    std::string error;
    assert(settings::parse_save(R"({"ssid":"Home","password":"12345678","pin":"123456","home":"Test Kitchen"})", error));

    // Off by default; enabling needs a key; odd characters rejected; key never echoed.
    assert(!geolocate::health().enabled && !geolocate::detect_now());
    assert(!geolocate::parse_save(R"({"geoEnabled":true})", error));
    assert(!geolocate::parse_save(R"({"geoEnabled":true,"geoApiKey":"AIza bad&key"})", error));
    assert(geolocate::parse_save(R"({"geoEnabled":true,"geoApiKey":"AIzaSyTEST_key-123"})", error));
    assert(geolocate::settings_json().find("AIza") == std::string::npos);
    assert(geolocate::settings_json().find("\"hasGeoApiKey\":true") != std::string::npos);

    fake_aps = {fake_ap("HomeNet", 1, -48, 6), fake_ap("Neighbour", 2, -67, 11), fake_ap("Cafe_nomap", 3, -60, 1),
                fake_ap("Shop", 4, -80, 3),fake_ap("Duplicate",1,-55,6),fake_ap("Random",5,-50,1)};
    fake_aps.back().bssid[0]=0xa6;
    fake_http_status = 200;
    fake_http_response = R"({"location":{"lat":18.9559,"lng":72.8117},"accuracy":38.5})";
    run_task(1); // first loop: lookup
    assert(fake_scans == 1 && fake_http_requests == 1);
    auto &req = fake_http_last;
    assert(req.url == "https://www.googleapis.com/geolocation/v1/geolocate?key=AIzaSyTEST_key-123");
    cJSON *body = cJSON_Parse(req.body.c_str());
    assert(body && cJSON_IsFalse(cJSON_GetObjectItem(body, "considerIp")));
    cJSON *aps = cJSON_GetObjectItem(body, "wifiAccessPoints");
    assert(cJSON_GetArraySize(aps) == 3); // _nomap network skipped
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(aps, 0), "macAddress")->valuestring, "a4:2b:b0:10:20:01"));
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(aps, 0), "signalStrength")->valueint == -48);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(aps, 1), "channel")->valueint == 11);
    cJSON_Delete(body);
    auto h = geolocate::health();
    assert(h.fix.valid && h.fix.lat == 18.9559 && h.fix.accuracyM == 38.5f && h.lastAps == 3);

    // No fixed location saved -> Wi-Fi fallback.
    auto r = geolocate::current();
    assert(r.valid && r.source == location_policy::WIFI);

    // Fixed location saved -> it wins; the Wi-Fi fix agrees (no warning).
    assert(settings::parse_save(R"({"hasLocation":true,"latitude":18.9561,"longitude":72.8119})", error));
    r = geolocate::current();
    assert(r.source == location_policy::MANUAL && r.lat == 18.9561 && !r.mismatch);
    // Wrong fixed pin (5 km away) -> still used, but flagged.
    assert(settings::parse_save(R"({"latitude":19.0,"longitude":72.8119})", error));
    assert(geolocate::current().mismatch);
    assert(geolocate::settings_json().find("\"mismatch\":true") != std::string::npos);

    // The cached fix survives a reboot.
    geolocate::init();
    assert(geolocate::health().fix.valid && geolocate::health().fix.lng == 72.8117);

    // Each run_task() restarts the task, like a reboot: one lookup at start-up, then nothing more for a day.
    run_task(4);
    assert(fake_scans == 2 && fake_http_requests == 2);

    // "Detect now": 30 s cooldown; blocked during an alarm.
    geolocate::set_alarm(true);
    assert(!geolocate::detect_now());
    geolocate::set_alarm(false);
    assert(geolocate::detect_now() && !geolocate::detect_now());

    // Errors keep the previous fix and are reported.
    fake_http_status = 403;
    fake_http_response = R"({"error":{"code":403,"message":"denied","status":"PERMISSION_DENIED"}})";
    run_task(1);
    assert(fake_scans == 3 && geolocate::health().failures == 1 && geolocate::health().fix.valid);
    assert(std::string(geolocate::health().lastError).find("403") != std::string::npos);

    // Too few networks: no HTTP request.
    fake_aps = {fake_ap("Lonely", 9, -70, 6)};
    fake_ticks += 31000;
    assert(geolocate::detect_now());
    run_task(1);
    assert(fake_scans == 4 && fake_http_requests == 3);

    // An alarm pauses even a pending request.
    fake_aps = {fake_ap("A", 1, -50, 1), fake_ap("B", 2, -55, 6)};
    fake_ticks += 31000;
    assert(geolocate::detect_now());
    geolocate::set_alarm(true);
    run_task(3);
    assert(fake_scans == 4);
    geolocate::set_alarm(false);
    fake_http_status = 200;
    fake_http_response = R"({"location":{"lat":18.9562,"lng":72.8120},"accuracy":22})";
    run_task(1);
    assert(fake_scans == 5 && geolocate::health().fix.accuracyM == 22.0f);

    // Disabling Wi-Fi geolocation stops use of the cache, without removing a saved fixed pin.
    assert(settings::parse_save(R"({"hasLocation":false})",error));
    assert(geolocate::current().source==location_policy::WIFI);
    assert(geolocate::parse_save(R"({"geoEnabled":false})",error));
    assert(!geolocate::current().valid);
    // Forget the cached fix.
    assert(geolocate::parse_save(R"({"geoForgetFix":true,"geoEnabled":false})", error));
    assert(!geolocate::health().fix.valid && !geolocate::detect_now());
    // A reply arriving after the user disables/forgets location cannot resurrect the cache.
    assert(geolocate::parse_save(R"({"geoEnabled":true})",error));
    fake_http_hook=[](){std::string e;assert(geolocate::parse_save(R"({"geoEnabled":false,"geoForgetFix":true})",e));};
    run_task(1);
    assert(!geolocate::health().enabled);
    assert(geolocate::parse_save(R"({"geoEnabled":true})",error));
    assert(!geolocate::health().fix.valid);
    std::cout << "Wi-Fi geolocation module tests passed\n";
}
