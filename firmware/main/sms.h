#pragma once
// SMS alerts through textbee (https://textbee.dev): an Android phone running the textbee app sends
// the SMS from its own SIM. The ESP32 makes one HTTPS request per alert; no extra hardware.
#include "alert_policy.h"
#include "location_policy.h"
#include <stdint.h>
#include <string>

namespace sms {
struct Health {
    bool enabled = false, configured = false;
    uint32_t sent = 0;    // SMS accepted by textbee (one per recipient)
    uint32_t failed = 0;  // batches that failed after retries
    uint32_t skipped = 0; // batches skipped by the per-event limit or a full queue
    int lastStatus = 0;   // last HTTP status from textbee (0 = no request yet / network error)
    bool station = false;     // fire-station SMS switched on (and SMS configured)
    uint32_t stationSent = 0; // fire-station SMS accepted by textbee
};
void init();  // load SMS options from NVS; call after nvs_flash_init()
void start(); // start the sender task
// Non-blocking; safe to call from alerts::observe(). Ignores alert kinds that are not sent by SMS.
// `policyEvent` groups alerts for the per-event limit; `shownEvent` is the /ack number shown in the SMS.
void notify(alert_policy::Kind kind, uint32_t policyEvent, uint32_t shownEvent, bool flame);
bool test(); // queue a test SMS to every resident (30 s cooldown)
// Fire-station SMS (hybrid location feature). Sent once per emergency event, only when SMS is on,
// the "Also SMS the fire-station number" switch is on and the fire-station number is a full phone
// number (short codes such as 101/112 cannot receive SMS and are rejected).
bool station_ready();
bool notify_station(uint32_t event, location_policy::StationReason why, bool testMode,
                    const location_policy::Resolved &where);
bool test_station(const location_policy::Resolved &where); // 30 s cooldown
std::string settings_json(); // never contains the API key
bool parse_save(const char *json, std::string &error);
Health health();
} // namespace sms
