// Host test for firmware/main/sms.cpp with stand-in ESP-IDF headers (tests/support).
// Checks settings handling and the exact HTTPS request sent to textbee.
#include "cJSON.h"
#include "esp_http_client.h"
#include "freertos/task.h"
#include "settings.h"
#include "nvs.h"
#include "sms.h"
#include "wifi_sync.h"
#include <assert.h>
#include <iostream>
#include <string.h>

namespace wifi_sync {
bool connected() { return true; }
} // namespace wifi_sync

// Runs the SMS task until it has been idle for `delays` waits.
static void run_task(int delays = 200) {
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
    sms::init();
    sms::start();
    assert(fake_task_fn);
    std::string error;
    assert(settings::parse_save(
        R"({"ssid":"Home","password":"12345678","pin":"123456","home":"Test Kitchen","contacts":[{"name":"Asha","phone":"98765 43210"},{"name":"Bad","phone":"12345"},{"name":"Ravi","phone":"+44 7700 900123"},{"name":"Dup","phone":"+919876543210"}]})",
        error));

    // Off by default; enabling needs a key; bad characters are rejected; the key is never echoed.
    assert(!sms::health().enabled && !sms::test());
    assert(!sms::parse_save(R"({"smsEnabled":true})", error));
    assert(!sms::parse_save(R"({"smsEnabled":true,"smsApiKey":"bad key\r\nx: y"})", error));
    assert(sms::parse_save(R"({"smsEnabled":true,"smsApiKey":"tb_SECRET-key.123","smsDeviceId":"dev42"})", error));
    assert(sms::parse_save(R"({"smsApiKey":""})", error)); // blank keeps the saved key
    auto js = sms::settings_json();
    assert(js.find("SECRET") == std::string::npos && js.find("\"hasSmsApiKey\":true") != std::string::npos);
    assert(js.find("+919876543210") != std::string::npos && js.find("+447700900123") != std::string::npos);
    assert(js.find("\"smsInvalid\":[\"Bad\"]") != std::string::npos);

    // Options survive a reboot (NVS reload).
    sms::init();
    assert(sms::health().enabled && sms::health().configured);

    // A FIRE alert produces one request with the de-duplicated, valid numbers.
    sms::notify(alert_policy::FIRE, 1, 7, true);
    sms::notify(alert_policy::UPDATE, 1, 7, true); // not an SMS kind
    sms::notify(alert_policy::FIRE, 1, 7, true);   // within 60 s: limited
    run_task();
    assert(fake_http_requests == 1);
    auto &req = fake_http_last;
    assert(req.url == "https://api.textbee.dev/api/v1/gateway/send-sms" && req.method == HTTP_METHOD_POST);
    assert(req.headers["x-api-key"] == "tb_SECRET-key.123" && req.headers["Content-Type"] == "application/json");
    cJSON *body = cJSON_Parse(req.body.c_str());
    assert(body);
    cJSON *to = cJSON_GetObjectItem(body, "recipients");
    assert(cJSON_GetArraySize(to) == 2);
    assert(!strcmp(cJSON_GetArrayItem(to, 0)->valuestring, "+919876543210"));
    assert(!strcmp(cJSON_GetArrayItem(to, 1)->valuestring, "+447700900123"));
    assert(!strcmp(cJSON_GetObjectItem(body, "deviceId")->valuestring, "dev42"));
    std::string message = cJSON_GetObjectItem(body, "message")->valuestring;
    assert(message.find("FIRE ALERT") != std::string::npos && message.find("Test Kitchen") != std::string::npos &&
           message.find("Ev 7") != std::string::npos && message.size() <= 160);
    std::cout << "Example SMS: " << message << "\n";
    cJSON_Delete(body);
    assert(sms::health().sent == 2 && sms::health().skipped == 1 && sms::health().lastStatus == 200);

    // All-clear after an alarm SMS.
    sms::notify(alert_policy::ALL_CLEAR, 1, 7, false);
    run_task();
    assert(fake_http_requests == 2 && fake_http_last.body.find("All clear") != std::string::npos);

    // Ambiguous server failures are not replayed (avoid duplicate SMS).
    fake_http_status = 503;
    sms::notify(alert_policy::GAS, 2, 8, false);
    run_task(400);
    assert(fake_http_requests == 3 && sms::health().failed == 1);

    // A bad API key (401) is not retried.
    fake_http_status = 401;
    sms::notify(alert_policy::FIRE, 3, 9, false);
    run_task();
    assert(fake_http_requests == 4 && sms::health().failed == 2 && sms::health().lastStatus == 401);

    // Test SMS, with a 30 s cooldown; textbee's older 201 reply also counts as success.
    fake_http_status = 201;
    assert(sms::test() && !sms::test());
    run_task();
    assert(fake_http_requests == 5 && fake_http_last.body.find("TEST ONLY") != std::string::npos);

    // No device ID: the field is omitted so textbee uses the default phone.
    assert(sms::parse_save(R"({"smsDeviceId":""})", error));
    fake_ticks += 31000;
    assert(sms::test());
    run_task();
    assert(fake_http_last.body.find("deviceId") == std::string::npos);

    // Removed CO never emits SMS.
    sms::notify(alert_policy::CO, 55, 55, false);
    run_task();
    assert(fake_http_requests == 6);

    // Turning SMS off stops new alerts.
    assert(sms::parse_save(R"({"smsEnabled":false})", error));
    sms::notify(alert_policy::FIRE, 4, 10, true);
    run_task();
    assert(fake_http_requests == 6);

    // ---- Fire-station SMS (hybrid location feature) ----
    location_policy::WifiFix fix;
    fix.valid = true; fix.lat = 18.95600; fix.lng = 72.81200; fix.accuracyM = 30;
    auto where = location_policy::resolve(false, 0, 0, fix);
    // Needs SMS on and a full phone number: short codes like 101 cannot receive SMS.
    assert(!sms::parse_save(R"({"stationSms":true})", error)); // SMS is off at this point
    assert(sms::parse_save(R"({"smsEnabled":true})", error));
    assert(settings::parse_save(R"({"station":"Andheri Fire Station","stationPhone":"101","landmark":"Near the temple"})", error));
    assert(!sms::parse_save(R"({"stationSms":true})", error) && error.find("101") != std::string::npos);
    assert(!sms::station_ready() && !sms::notify_station(21, location_policy::STATION_FIRE, false, where));
    assert(settings::parse_save(R"({"stationPhone":"+91 90000 11111","emergencyEnabled":true})", error));
    assert(sms::parse_save(R"({"stationSms":true})", error));
    assert(sms::settings_json().find("\"stationSms\":true") != std::string::npos);
    sms::init(); // survives a reboot
    assert(sms::health().station);
    fake_ticks += 6000; // past the 5 s readiness cache
    assert(sms::station_ready());
    int before = fake_http_requests;
    assert(sms::notify_station(21, location_policy::STATION_FIRE, true, where));
    assert(sms::notify_station(21, location_policy::STATION_FIRE, true, where)); // once per event
    run_task();
    assert(fake_http_requests == before + 1);
    body = cJSON_Parse(fake_http_last.body.c_str());
    to = cJSON_GetObjectItem(body, "recipients");
    assert(cJSON_GetArraySize(to) == 1 && !strcmp(cJSON_GetArrayItem(to, 0)->valuestring, "+919000011111"));
    message = cJSON_GetObjectItem(body, "message")->valuestring;
    std::cout << "Example fire-station SMS: " << message << "\n";
    assert(message.rfind("TEST MODE - FIRE alert", 0) == 0);
    assert(message.find("Map: https://maps.google.com/?q=18.95600,72.81200 (+-30m)") != std::string::npos);
    assert(message.find("Landmark: Near the temple") != std::string::npos && message.find("Resident: +919876543210") != std::string::npos);
    assert(message.find("Ev 21") != std::string::npos && message.size() <= 306);
    cJSON_Delete(body);
    assert(sms::health().stationSent == 1);
    // A new emergency event is sent again; the setup-page test is marked TEST ONLY.
    assert(sms::notify_station(22, location_policy::STATION_UNATTENDED, false, where));
    run_task();
    assert(fake_http_last.body.find("Unanswered FireGuard warning") != std::string::npos);
    assert(sms::test_station(where) && !sms::test_station(where));
    run_task();
    assert(fake_http_last.body.find("TEST ONLY") != std::string::npos && sms::health().stationSent == 3);
    // Turning the switch off stops it.
    assert(sms::parse_save(R"({"stationSms":false})", error));
    assert(!sms::station_ready() && !sms::notify_station(23, location_policy::STATION_FIRE, false, where));
    // A full queue must not consume an emergency event; accepting it later remains possible.
    assert(sms::parse_save(R"({"stationSms":true})",error));
    for(unsigned n=100;n<108;++n)sms::notify(alert_policy::FIRE,n,n,false);
    assert(!sms::notify_station(999,location_policy::STATION_FIRE,true,where));
    run_task(100);
    assert(sms::notify_station(999,location_policy::STATION_FIRE,true,where));
    run_task();
    // Destination is frozen at enqueue, not taken from subsequently edited settings.
    assert(sms::notify_station(1000,location_policy::STATION_FIRE,true,where));
    assert(settings::parse_save(R"({"stationPhone":"+91 98888 22222"})",error));
    run_task();
    assert(fake_http_last.body.find("+919000011111")!=std::string::npos);
    assert(fake_http_last.body.find("+919888822222")==std::string::npos);
    // Disabling the station switch cancels its queued sends.
    before=fake_http_requests;
    assert(sms::notify_station(1001,location_policy::STATION_FIRE,true,where));
    assert(sms::parse_save(R"({"stationSms":false})",error));
    run_task();assert(fake_http_requests==before);
    std::cout << "SMS module tests passed\n";
}
