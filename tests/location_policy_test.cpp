// Host tests for the pure hybrid-location rules: python3 tests/run.py
#include "location_policy.h"
#include <assert.h>
#include <iostream>
using namespace location_policy;

int main() {
    // Distances (Mumbai CST -> Gateway of India is about 1.9 km).
    double d = distance_m(18.94020, 72.83560, 18.92200, 72.83470);
    assert(d > 1900 && d < 2150);
    assert(distance_m(19.0, 72.8, 19.0, 72.8) < 0.001);

    WifiFix none;
    WifiFix near;
    near.valid = true; near.lat = 18.95600; near.lng = 72.81200; near.accuracyM = 35; near.when = 1;
    WifiFix far = near;
    far.lat = 19.00000; // ~4.9 km north
    WifiFix vague = near;
    vague.accuracyM = 5000; // neighbourhood-level only

    // 1. Fixed location wins when saved; no Wi-Fi needed.
    auto r = resolve(true, 18.95580, 72.81190, none);
    assert(r.valid && r.source == MANUAL && r.lat == 18.95580 && !r.mismatch);
    // 2. Fixed + agreeing Wi-Fi: still fixed, no warning.
    r = resolve(true, 18.95580, 72.81190, near);
    assert(r.valid && r.source == MANUAL && !r.mismatch && r.mismatchM < 100);
    // 3. Fixed + disagreeing Wi-Fi: fixed is used but flagged.
    r = resolve(true, 18.95580, 72.81190, far);
    assert(r.source == MANUAL && r.mismatch && r.mismatchM > 4000);
    // 4. No fixed location: Wi-Fi fallback.
    r = resolve(false, 0, 0, near);
    assert(r.valid && r.source == WIFI && r.accuracyM == 35);
    // "Use saved location" ticked but left at 0,0 counts as not saved.
    r = resolve(true, 0, 0, near);
    assert(r.source == WIFI);
    // 5. Nothing usable.
    assert(!resolve(false, 0, 0, none).valid);
    assert(!resolve(false, 0, 0, vague).valid); // too vague on its own
    assert(!resolve(true, 91, 0, none).valid);

    // Disagreement threshold: never below 300 m, otherwise 3 x accuracy.
    assert(!disagrees(250, 20) && disagrees(350, 20));
    assert(!disagrees(500, 200) && disagrees(700, 200));

    // Wi-Fi fallback has a bounded lifetime; manual location always wins even with a stale fix.
    near.when=1800000000;
    assert(resolve(false,0,0,near,near.when+48*3600).valid);
    assert(!resolve(false,0,0,near,near.when+48*3600+1).valid);
    assert(!resolve(false,0,0,near,near.when-301).valid);
    assert(resolve(true,18,72,near,near.when+49*3600).source==MANUAL);
    // Refresh schedule: never during an alarm; daily with a fix; 10-minute retries without.
    assert(!should_refresh(false, false, false, false, 0));
    assert(!should_refresh(true, true, false, false, 0));
    assert(should_refresh(true, false, false, false, 0));
    assert(!should_refresh(true, false, false, true, RETRY_MS - 1) && should_refresh(true, false, false, true, RETRY_MS));
    assert(!should_refresh(true, false, true, true, RETRY_MS) && should_refresh(true, false, true, true, REFRESH_MS));

    // Telegram line.
    auto line = telegram_line(resolve(true, 18.95580, 72.81190, far));
    assert(line.find("https://maps.google.com/?q=18.95580,72.81190") != std::string::npos);
    assert(line.find("installation") != std::string::npos && line.find("verify the address") != std::string::npos);
    line = telegram_line(resolve(false, 0, 0, near));
    assert(line.find("Wi-Fi estimate, about 35 m") != std::string::npos);
    assert(telegram_line(Resolved{}).empty());

    // Fire-station SMS: <= 2 SMS parts, plain ASCII, address + map + call-back + event.
    std::string longAddr(300, 'A');
    for (auto why : {STATION_FIRE, STATION_UNATTENDED, STATION_TEST})
        for (bool test : {false, true}) {
            auto t = station_text(why, test, "Home", longAddr, std::string(80, 'L'), "9876543210",
                                  resolve(false, 0, 0, near), 4294967295u,"400058");
            assert(t.size() <= STATION_SMS_MAX);
            assert(t.find("Postal: 400058")!=std::string::npos);
            assert(t.find("Resident: +919876543210")!=std::string::npos);
            assert(t.find("Map: https://maps.google.com/?q=18.95600,72.81200")!=std::string::npos);
            if(why!=STATION_TEST)assert(t.find("Ev 4294967295")!=std::string::npos);
            for (unsigned char c : t)
                assert(c >= 32 && c < 127);
        }
    auto fire = station_text(STATION_FIRE, false, "Our Home", "Flat 4, Sai Krupa, Station Rd, Andheri West 400058",
                             "Opp. post office", "98765 43210", resolve(true, 18.95580, 72.81190, none), 12);
    std::cout << "Example fire-station SMS (" << fire.size() << " chars): " << fire << "\n";
    assert(fire.rfind("FIRE alert", 0) == 0 && fire.find("Sai Krupa") != std::string::npos);
    assert(fire.find("Landmark: Opp. post office") != std::string::npos);
    assert(fire.find("Map: https://maps.google.com/?q=18.95580,72.81190") != std::string::npos);
    assert(fire.find("Resident: +919876543210") != std::string::npos && fire.find("Ev 12") != std::string::npos);
    assert(fire.find("(+-") == std::string::npos); // no accuracy note for the exact fixed location
    auto testMode = station_text(STATION_UNATTENDED, true, "H", "Addr", "", "", resolve(false, 0, 0, near), 3);
    assert(testMode.rfind("TEST MODE - Unanswered", 0) == 0 && testMode.find("(+-35m)") != std::string::npos);
    assert(testMode.find("Resident") == std::string::npos);
    auto check = station_text(STATION_TEST, false, "Home", "", "", "", Resolved{}, 9);
    assert(check.rfind("TEST ONLY", 0) == 0 && check.find("Home") != std::string::npos &&
           check.find("Map") == std::string::npos && check.find("Ev ") == std::string::npos);
    std::cout << "Location policy tests passed\n";
}
