#pragma once
#include <stdint.h>
#include <string>
namespace settings {
constexpr int MAX_CONTACTS = 8;
struct Address {
    char unit[24] = {}, building[40] = {}, street[64] = {}, area[40] = {};
    char city[40] = {}, state[40] = {}, postalCode[16] = {}, country[40] = {};
};
std::string format_address(const Address &a);
struct Contact {
    char name[48] = {}, phone[32] = {}, address[384] = {};
    int64_t chat = 0;
    Address addressFields;
};
struct Config {
    uint32_t version = 4;
    char ssid[33] = {}, password[65] = {}, token[128] = {}, pin[33] = {};
    char home[64] = {}, address[384] = {}, landmark[128] = {}, station[64] = {}, stationPhone[32] = {};
    double latitude = 0, longitude = 0;
    bool hasLocation = false;
    Contact contacts[MAX_CONTACTS];
    Address addressFields;
    bool cloudEnabled = false;
    char cloudUrl[192] = {}, cloudDeviceToken[96] = {};
    char setupPassword[17] = {};
    char ambulancePhone[32] = {}, emergencyPhone[32] = {}, emergencyName[48] = {}, emergencyPin[33] = {};
    int64_t emergencyChat = 0;
    bool emergencyEnabled = false, emergencyTest = true;
    uint32_t unattendedSeconds = 60;
    float fireConfidence = 0.85f;
};
struct AlertOptions {
    bool enabled, test, recipient;
    uint32_t unattendedSeconds;
    float fireConfidence;
    int64_t chat;
};
AlertOptions alert_options();
struct CloudOptions {
    bool enabled;
    char url[192], token[96];
};
CloudOptions cloud_options();
int registered_count();
bool has_wifi();
void init();
Config get();
bool save(const Config &c);
// Private Telegram chat IDs only. A PIN joins an existing slot or the first free one.
bool join(int64_t chat, const char *name);
bool join_emergency(int64_t chat, const char *name);
std::string public_json(bool installation = false);
bool parse_save(const char *json, std::string &error, bool *networkChanged = nullptr);
} // namespace settings
