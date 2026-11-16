#include "cJSON.h"
#include "settings.h"
#include <assert.h>
#include <iostream>
#include <string.h>
using namespace settings;
int main() {
    init();
    std::string error;
    assert(get().version == 4 && strlen(get().pin) >= 6);
    assert(parse_save(
        R"({"ssid":"Home","password":"12345678","token":"123:ABC","pin":"123456","home":"Kitchen","address":"Home address","contacts":[{"name":"Resident","phone":"12345","address":"Resident address"}]})",
        error));
    bool networkChanged = true;
    assert(parse_save(R"({"home":"Kitchen updated","password":"","token":""})", error, &networkChanged));
    assert(!networkChanged);
    assert(parse_save(R"({"ssid":"Other Home"})", error, &networkChanged) && networkChanged);
    assert(parse_save(R"({"password":"87654321"})", error, &networkChanged) && networkChanged);
    assert(!parse_save(R"({"password":"short"})", error, &networkChanged) && !networkChanged);
    assert(parse_save(R"({"ssid":"Home","password":"12345678"})", error, &networkChanged) && networkChanged);
    auto c = get();
    assert(!strcmp(c.address, "Home address") && !strcmp(c.contacts[0].address, "Resident address"));
    assert(join(123, "Different name") && join(123, "Same user"));
    assert(get().contacts[0].chat == 123 && !strcmp(get().contacts[0].name, "Resident") &&
           get().contacts[1].chat == 0);
    auto pub = public_json(false);
    assert(pub.find("123:ABC") == std::string::npos && pub.find("12345678") == std::string::npos &&
           pub.find("Resident address") != std::string::npos);
    assert(
        parse_save(R"({"password":"","token":"","contacts":[{"address":"New resident address"}]})", error));
    assert(!strcmp(get().password, "12345678") && get().contacts[0].chat == 123 &&
           !strcmp(get().contacts[0].address, "New resident address"));
    assert(!parse_save(R"({"contacts":[{"address":123}]})", error));
    std::string longAddr = "{\"contacts\":[{\"address\":\"" + std::string(384, 'a') + "\"}]}";
    assert(!parse_save(longAddr.c_str(), error));
    assert(!strcmp(get().contacts[0].address, "New resident address"));
    assert(parse_save(R"({"contacts":[{"remove":true}]})", error));
    assert(get().contacts[0].chat == 0);
    assert(join(123, "Resident"));
    assert(parse_save(R"({"token":"456:DEF"})", error));
    assert(get().contacts[0].chat == 0);
    for (int64_t id = 1; id <= 8; id++)
        assert(join(id, "Resident"));
    assert(!join(9, "Overflow"));
    assert(!parse_save(R"({"latitude":91,"longitude":20})", error));
    assert(!parse_save(R"({"pin":"abc"})", error));
    assert(parse_save(
        R"({"addressFields":{"unit":"12B","street":"Example Road","city":"Example City","state":"Region","postalCode":"123456","country":"Example Country"},"ambulancePhone":"TEST-100","emergencyPhone":"TEST-200","emergencyEnabled":true,"emergencyTest":true,"unattendedSeconds":60,"fireConfidence":0.85,"contacts":[{"addressFields":{"unit":"2","city":"Resident City","postalCode":"654321","country":"Example Country"}}]})",
        error));
    assert(std::string(get().address).find("123456") != std::string::npos);
    assert(std::string(get().contacts[0].address).find("Resident City") != std::string::npos);
    assert(!strcmp(get().ambulancePhone, "TEST-100"));
    assert(join_emergency(999, "Test recipient"));
    assert(alert_options().recipient && alert_options().chat == 999);
    assert(parse_save(R"({"home":"Renamed"})", error));
    assert(get().emergencyChat == 999);
    assert(parse_save(R"({"removeEmergency":true})", error));
    assert(!get().emergencyChat);
    assert(!parse_save(R"({"unattendedSeconds":29})", error));
    assert(!parse_save(R"({"fireConfidence":0.3})", error));
    assert(!parse_save(R"({"cloudEnabled":true,"cloudUrl":"http://invalid","cloudDeviceToken":"device-secret"})",error));
    assert(parse_save(R"({"cloudEnabled":true,"cloudUrl":"https://example.com/","cloudDeviceToken":"device-secret"})",error));
    assert(cloud_options().enabled&&!strcmp(cloud_options().token,"device-secret"));
    assert(public_json(true).find("device-secret")==std::string::npos);
    std::cout << "Config bytes: " << sizeof(Config) << "\n";
    std::cout << "Resident address persistence, validation, secret retention and registration tests passed\n";
}
