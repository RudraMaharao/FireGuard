#pragma once
#include <stdint.h>
namespace alert_policy {
enum Kind {
    NONE,
    FIRE,
    POSSIBLE,
    GAS,
    CO,
    UPDATE,
    ESCALATE,
    ALL_CLEAR,
    TEST,
    WARNING,
    EMERGENCY_UNATTENDED,
    EMERGENCY_HIGH,
    EMERGENCY_TEST,
    READINGS,
    SNAPSHOT
};
struct Input {
    uint8_t state;
    bool flame, muted;
    uint32_t now;
};
// Numeric states match the unchanged fusion.h enum. Pure policy for host tests.
inline Kind hazard(uint8_t state) {
    switch (state) {
    case 3:
    case 4:
    case 5:
        return WARNING;
    case 9:
    case 10:
        return FIRE;
    case 8:
        return POSSIBLE;
    case 6:
        return GAS;
    case 7:
        return CO;
    default:
        return NONE;
    }
}
struct Intent {
    Kind kind = NONE;
    uint32_t event = 0;
};
class Policy {
    bool active = false, wasFlame = false, escalated = false, scoreOnly = false;
    Kind previous = NONE;
    uint32_t event = 0, last = 0, scoreSince = 0;
    unsigned rounds = 0;

  public:
    static constexpr unsigned MAX_ROUNDS = 8; // + one all-clear, per registered contact
    Intent next(Input i) const {
        Kind h = hazard(i.state);
        if (active && i.state == 1)
            return {ALL_CLEAR, event};
        if (!active && h != NONE)
            return {h, event + 1};
        if (!active || rounds >= MAX_ROUNDS)
            return {};
        if (h != NONE && (h != previous || (h == FIRE && i.flame && !wasFlame)))
            return {h, event};
        if (h == FIRE && !i.flame && scoreOnly && !escalated && !i.muted && i.now - scoreSince >= 60000)
            return {ESCALATE, event};
        if (h != NONE && i.now - last >= 180000)
            return {UPDATE, event};
        return {};
    }
    void accepted(Intent n, Input i) {
        Kind h = hazard(i.state);
        if (n.kind == ALL_CLEAR) {
            active = false;
            previous = NONE;
            rounds = 0;
            scoreOnly = false;
            return;
        }
        if (!active) {
            active = true;
            event = n.event;
            rounds = 0;
            escalated = false;
        }
        ++rounds;
        last = i.now;
        previous = h;
        wasFlame = i.flame;
        if (n.kind == ESCALATE)
            escalated = true;
    }
    void track(Input i) {
        bool score = hazard(i.state) == FIRE && !i.flame;
        if (score && !scoreOnly) {
            scoreSince = i.now;
            escalated = false;
        }
        scoreOnly = score;
    }
};
} // namespace alert_policy
