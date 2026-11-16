#pragma once
// Pure SMS rules (no ESP-IDF calls) so they can be unit-tested on a PC: which alerts become SMS,
// how phone numbers are normalised, and the short single-SMS text.
#include "alert_policy.h"
#include <stdint.h>
#include <stdio.h>
#include <string>

namespace sms_policy {
using alert_policy::Kind;

// textbee counts every recipient as one message (free plan: 50/day, 300/month), so only
// high-severity prototype alerts are sent by SMS. Periodic updates, warnings and readings stay on Telegram.
inline bool wants_sms(Kind k) {
    return k == alert_policy::FIRE || k == alert_policy::GAS ||
           k == alert_policy::ESCALATE || k == alert_policy::ALL_CLEAR;
}

// At most this many SMS batches per alert event (e.g. FIRE, flame-confirmed FIRE, ESCALATE, ALL_CLEAR).
constexpr unsigned MAX_BATCHES_PER_EVENT = 4;
// Never send two alert batches closer than this (all-clear and tests are exempt).
constexpr uint32_t MIN_GAP_MS = 60000;

class Limiter {
    uint32_t event = 0, last = 0;
    unsigned batches = 0;
    bool sentForEvent = false;

  public:
    // Returns true when an SMS batch for (kind, event) may be queued now.
    bool allow(Kind k, uint32_t ev, uint32_t now) const {
        if (k == alert_policy::ALL_CLEAR)
            return sentForEvent && ev == event; // only tell people "all clear" if they got an alarm SMS
        if (ev != event)
            return true; // a new alarm event is always sent
        if (batches >= MAX_BATCHES_PER_EVENT - 1)
            return false;
        return now - last >= MIN_GAP_MS;
    }
    void accepted(Kind k, uint32_t ev, uint32_t now) {
        if (k == alert_policy::ALL_CLEAR) {
            sentForEvent = false;
            batches = 0;
            return;
        }
        if (ev != event) {
            event = ev;
            batches = 0;
        }
        ++batches;
        last = now;
        sentForEvent = true;
    }
};

// Returns "+<digits>" or "" if the number cannot be used. Spaces, dashes, dots and brackets are
// ignored. A bare 10-digit Indian mobile number (starting 6-9), "0" + 10 digits, or "91" + 10 digits
// gets the +91 prefix. Any other number must already include its "+country" code.
inline std::string normalise_phone(const char *raw) {
    std::string digits;
    bool plus = false;
    for (const char *p = raw; *p; p++) {
        char c = *p;
        if (c == '+' && digits.empty() && !plus)
            plus = true;
        else if (c >= '0' && c <= '9')
            digits += c;
        else if (c == ' ' || c == '-' || c == '.' || c == '(' || c == ')')
            continue;
        else
            return "";
    }
    if (plus)
        return digits.size() >= 8 && digits.size() <= 15 && digits[0] != '0' ? "+" + digits : "";
    if (digits.size() == 11 && digits[0] == '0')
        digits.erase(0, 1);
    if (digits.size() == 12 && digits.compare(0, 2, "91") == 0)
        digits.erase(0, 2);
    if (digits.size() == 10 && digits[0] >= '6' && digits[0] <= '9')
        return "+91" + digits;
    return "";
}

// Keeps the text in the GSM-7 basic set (plain ASCII) so one SMS holds 160 characters.
// Any other character (emoji, accented or non-Latin letters) would force 70-character UCS-2 parts.
inline std::string ascii(const std::string &in, size_t max) {
    std::string out;
    for (unsigned char c : in) {
        if (out.size() >= max)
            break;
        if (c >= 0x80) {
            // Skip UTF-8 continuation bytes; replace each multi-byte character with one '?'.
            if ((c & 0xC0) == 0x80)
                continue;
            out += '?';
        } else if (c < 32 || c == '`' || c == '^' || c == '{' || c == '}' || c == '[' || c == ']' ||
                   c == '~' || c == '|' || c == '\\')
            out += ' ';
        else
            out += char(c);
    }
    return out;
}

// One SMS (<= 160 characters). `home` is the configured home name; `flame` means fresh camera
// evidence of open flame; `test` marks a delivery check.
inline std::string text(Kind k, const std::string &home, uint32_t event, bool flame, bool test) {
    std::string where = ascii(home.empty() ? std::string("your home") : home, 28);
    const char *body;
    if (test)
        body = "TEST ONLY: SMS alerts are working. No action needed.";
    else
        switch (k) {
        case alert_policy::FIRE:
            body = flame ? "FIRE ALERT: camera detects flame. Leave now; seek emergency help."
                         : "FIRE ALERT: high smoke/heat score. Check immediately.";
            break;
        case alert_policy::ESCALATE:
            body = "FIRE ALARM STILL ACTIVE. Seek emergency help now.";
            break;
        case alert_policy::GAS:
            body = "GAS ALERT: gas/smoke activity. Check now; avoid flames and switches.";
            break;
        case alert_policy::CO:
            body = "CO ALERT: carbon monoxide risk. Move to fresh air now.";
            break;
        case alert_policy::ALL_CLEAR:
            body = "All clear: readings back to SAFE. Check before going back in.";
            break;
        default:
            body = "Alert. Check the home.";
            break;
        }
    char out[200];
    if (event && !test)
        snprintf(out, sizeof out, "FireGuard (%s): %s Ev %lu. Details on Telegram.", where.c_str(), body,
                 (unsigned long)event);
    else
        snprintf(out, sizeof out, "FireGuard (%s): %s", where.c_str(), body);
    std::string s = out;
    if (s.size() > 160)
        s.resize(160);
    return s;
}
} // namespace sms_policy
