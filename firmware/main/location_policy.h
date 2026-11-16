#pragma once
// Hybrid location rules (pure C++, no ESP-IDF calls) so they can be unit-tested on a PC.
//
//   1. The installer's fixed location (setup step 02) is the PRIMARY source: exact building, free,
//      and it cannot fail during a fire.
//   2. A Wi-Fi geolocation fix (nearby access points -> Google Geolocation API) is the FALLBACK
//      when no fixed location was saved, and a CROSS-CHECK when one was: if the two disagree by more
//      than the Wi-Fi accuracy allows, alerts say so, so a wrong pin is noticed.
//
// The Wi-Fi fix is looked up at boot and once a day while SAFE, then cached in flash. Alerts only
// read the cache; nothing is looked up during a fire.
#include "sms_policy.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

namespace location_policy {

enum Source { NONE = 0, MANUAL = 1, WIFI = 2 };
inline const char *source_name(Source s) { return s == MANUAL ? "fixed" : s == WIFI ? "wifi" : "none"; }

struct WifiFix {
    bool valid = false;
    double lat = 0, lng = 0;
    float accuracyM = 0; // radius reported by the API (68 % confidence)
    int64_t when = 0;    // UNIX time of the lookup
};

struct Resolved {
    bool valid = false;
    Source source = NONE;
    double lat = 0, lng = 0;
    float accuracyM = 0; // 0 for the fixed location
    bool mismatch = false;
    double mismatchM = 0; // distance between the fixed location and the Wi-Fi estimate
};

inline bool usable(double lat, double lng) {
    return isfinite(lat) && isfinite(lng) && fabs(lat) <= 90 && fabs(lng) <= 180 && !(lat == 0 && lng == 0);
}

// Great-circle distance in metres.
inline double distance_m(double lat1, double lng1, double lat2, double lng2) {
    constexpr double R = 6371000.0, D = M_PI / 180.0;
    double dlat = (lat2 - lat1) * D, dlng = (lng2 - lng1) * D;
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * D) * cos(lat2 * D) * sin(dlng / 2) * sin(dlng / 2);
    a=fmax(0.0,fmin(1.0,a));
    return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

// A Wi-Fi estimate counts as disagreeing when it is further from the fixed point than three
// accuracy radii, and never closer than 300 m (dense cities still give 20-50 m jitter).
inline bool disagrees(double distanceM, float accuracyM) {
    double limit = 3.0 * (accuracyM > 0 ? accuracyM : 100.0);
    return distanceM > (limit < 300 ? 300 : limit);
}

// Wi-Fi fixes worse than this are not used on their own (it would only point at the neighbourhood).
constexpr float MAX_WIFI_ACCURACY_M = 2000;

constexpr int64_t MAX_FIX_AGE_S=48*60*60;
inline bool fresh(const WifiFix &w,int64_t now) {
    return now>=1700000000 && w.when>=1700000000 && w.when<=now+300 && now-w.when<=MAX_FIX_AGE_S;
}
inline Resolved resolve(bool manualOn, double mLat, double mLng, const WifiFix &w, int64_t now=0) {
    Resolved r;
    bool wifiOk = w.valid && usable(w.lat, w.lng) && w.accuracyM > 0 && isfinite(w.accuracyM) && w.accuracyM <= MAX_WIFI_ACCURACY_M && (!now || fresh(w,now));
    if (manualOn && usable(mLat, mLng)) {
        r.valid = true;
        r.source = MANUAL;
        r.lat = mLat;
        r.lng = mLng;
        if (wifiOk) {
            r.mismatchM = distance_m(mLat, mLng, w.lat, w.lng);
            r.mismatch = disagrees(r.mismatchM, w.accuracyM);
        }
        return r;
    }
    if (wifiOk) {
        r.valid = true;
        r.source = WIFI;
        r.lat = w.lat;
        r.lng = w.lng;
        r.accuracyM = w.accuracyM;
    }
    return r;
}

// Refresh schedule for the background lookup.
constexpr uint32_t REFRESH_MS = 24UL * 60 * 60 * 1000; // daily while a fix exists
constexpr uint32_t RETRY_MS = 10UL * 60 * 1000;        // every 10 min until the first fix
inline bool should_refresh(bool enabled, bool alarm, bool haveFix, bool everTried, uint32_t sinceLastMs) {
    if (!enabled || alarm)
        return false; // never scan Wi-Fi (or spend time on HTTP) while an alarm is active
    if (!everTried)
        return true;
    return sinceLastMs >= (haveFix ? REFRESH_MS : RETRY_MS);
}

// 5 decimals is ~1 m; shorter links keep the SMS small.
inline std::string maps_link(double lat, double lng) {
    char b[64];
    snprintf(b, sizeof b, "https://maps.google.com/?q=%.5f,%.5f", lat, lng);
    return b;
}

// One line for Telegram alerts.
inline std::string telegram_line(const Resolved &r) {
    if (!r.valid)
        return "";
    std::string out = "\nMap: " + maps_link(r.lat, r.lng);
    char b[160];
    if (r.source == MANUAL)
        out += " (location saved at installation)";
    else {
        snprintf(b, sizeof b, " (Wi-Fi estimate, about %.0f m; use the address above to find the door)",
                 double(r.accuracyM));
        out += b;
    }
    if (r.mismatch) {
        snprintf(b, sizeof b, "\nNote: Wi-Fi places the device %.1f km from the saved location; verify the address.",
                 r.mismatchM / 1000.0);
        out += b;
    }
    return out;
}

enum StationReason { STATION_FIRE = 0, STATION_UNATTENDED = 1, STATION_TEST = 2 };

// SMS for the fire-station / emergency number. Up to two SMS parts (306 GSM-7 characters):
// completeness matters more than cost here. Order = what a dispatcher needs first.
constexpr size_t STATION_SMS_MAX = 306;
inline std::string station_text(StationReason why, bool testMode, const std::string &home,
                                 const std::string &address, const std::string &landmark,
                                 const std::string &callback, const Resolved &r, uint32_t event, const std::string &postal="") {
    using sms_policy::ascii;
    std::string prefix;
    if (why==STATION_TEST) prefix="TEST ONLY - FireGuard fire-station SMS check. No action needed. ";
    else {
        if(testMode) prefix="TEST MODE - ";
        prefix+=why==STATION_FIRE?"FIRE alert (high-confidence camera) at ":"Unanswered FireGuard warning at ";
    }
    std::string suffix;
    if(r.valid){
        suffix=" Map: "+maps_link(r.lat,r.lng);
        if(r.source==WIFI){char b[24];snprintf(b,sizeof b," (+-%.0fm)",double(r.accuracyM));suffix+=b;}
        if(r.mismatch)suffix+=" Verify saved pin.";
    }
    if(!postal.empty())suffix+=" Postal: "+ascii(postal,15)+".";
    auto phone=sms_policy::normalise_phone(callback.c_str());
    if(!phone.empty())suffix+=" Resident: "+phone+".";
    if(event&&why!=STATION_TEST)suffix+=" Ev "+std::to_string(event)+".";
    suffix+=" Auto alert, FireGuard.";
    // Reserve the complete map URL, callback and event before truncating address/landmark.
    size_t budget=STATION_SMS_MAX-prefix.size()-suffix.size()-1;
    std::string addr=ascii(address.empty()?home:address,budget<120?budget:120);
    std::string extra;
    if(!landmark.empty()&&budget>addr.size()+13){
        size_t room=budget-addr.size()-12;
        extra=". Landmark: "+ascii(landmark,room<40?room:40);
    }
    std::string out=prefix+addr+extra+"."+suffix;
    return out;
}

} // namespace location_policy
