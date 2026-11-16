// Host tests for the pure SMS rules: python3 tests/run.py
#include "sms_policy.h"
#include <assert.h>
#include <iostream>
using namespace alert_policy;
using sms_policy::normalise_phone;

int main() {
    // Only safety-critical alerts become SMS.
    for (Kind k : {FIRE, GAS, ESCALATE, ALL_CLEAR})
        assert(sms_policy::wants_sms(k));
    for (Kind k : {CO, NONE, POSSIBLE, UPDATE, TEST, WARNING, READINGS, EMERGENCY_HIGH, EMERGENCY_TEST})
        assert(!sms_policy::wants_sms(k));

    // Phone numbers.
    assert(normalise_phone("9876543210") == "+919876543210");
    assert(normalise_phone("98765 43210") == "+919876543210");
    assert(normalise_phone("098765-43210") == "+919876543210");
    assert(normalise_phone("919876543210") == "+919876543210");
    assert(normalise_phone("+91 98765 43210") == "+919876543210");
    assert(normalise_phone("+1 (202) 555-0123") == "+12025550123");
    assert(normalise_phone("12345").empty());          // too short
    assert(normalise_phone("5876543210").empty());     // not an Indian mobile prefix
    assert(normalise_phone("98765x43210").empty());    // letters
    assert(normalise_phone("+0123456789").empty());    // country code cannot start with 0
    assert(normalise_phone("").empty());

    // Messages fit one SMS and stay in plain ASCII (GSM-7).
    std::string longHome(80, 'H');
    for (Kind k : {FIRE, GAS, ESCALATE, ALL_CLEAR})
        for (bool flame : {false, true}) {
            auto t = sms_policy::text(k, longHome, 4294967295u, flame, false);
            assert(t.size() <= 160);
            for (unsigned char c : t)
                assert(c >= 32 && c < 127 && c != '[' && c != ']');
        }
    auto fire = sms_policy::text(FIRE, "Our Kitchen", 12, true, false);
    assert(fire.find("FIRE ALERT") != std::string::npos && fire.find("Ev 12") != std::string::npos &&
           fire.find("Our Kitchen") != std::string::npos);
    auto test = sms_policy::text(TEST, "Home \xF0\x9F\x94\xA5", 0, false, true); // emoji in the name
    assert(test.find("TEST ONLY") != std::string::npos && test.find("Home ?") != std::string::npos);
    assert(sms_policy::text(FIRE, "", 3, false, false).find("(your home)") != std::string::npos);

    // Per-event limiter.
    sms_policy::Limiter l;
    uint32_t t = 1000;
    assert(!l.allow(ALL_CLEAR, 1, t)); // no alarm SMS yet, so no all-clear SMS
    assert(l.allow(FIRE, 1, t));
    l.accepted(FIRE, 1, t);
    assert(!l.allow(FIRE, 1, t + 1000));           // 60 s gap within one event
    assert(l.allow(ESCALATE, 1, t + 60000));
    l.accepted(ESCALATE, 1, t + 60000);
    l.accepted(FIRE, 1, t + 120000);
    assert(!l.allow(FIRE, 1, t + 180000));        // reserve the fourth batch for all-clear
    assert(!l.allow(FIRE, 1, t + 600000));         // per-event cap
    assert(l.allow(ALL_CLEAR, 1, t + 600000));
    l.accepted(ALL_CLEAR, 1, t + 600000);
    assert(!l.allow(ALL_CLEAR, 1, t + 700000));    // only one all-clear
    assert(l.allow(GAS, 2, t + 600001));           // a new event is sent immediately
    std::cout << "SMS policy tests passed\n";
}
