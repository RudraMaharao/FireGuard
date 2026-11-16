#pragma once
#include <stdint.h>
namespace emergency_policy {
enum Reason { NONE, UNATTENDED, HIGH_FIRE };
class Policy {
    bool active_ = false, ack_ = false, forwarded_ = false;
    uint32_t id_ = 0, since_ = 0;

  public:
    explicit Policy(uint32_t seed = 0) : id_(seed) {}
    void track(bool warning, bool highFire, bool safe, uint32_t now) {
        if (safe && !highFire) {
            active_ = ack_ = forwarded_ = false;
            return;
        }
        if (!active_ && (warning || highFire)) {
            active_ = true;
            ack_ = forwarded_ = false;
            since_ = now;
            if (++id_ == 0)
                ++id_;
        }
    }
    bool acknowledge(uint32_t id) {
        if (!active_ || id != id_)
            return false;
        ack_ = true;
        return true;
    }
    Reason next(bool enabled, bool recipient, bool highFire, uint32_t timeoutMs, uint32_t now) const {
        if (!active_ || forwarded_ || !enabled || !recipient)
            return NONE;
        if (highFire)
            return HIGH_FIRE;
        if (!ack_ && now - since_ >= timeoutMs)
            return UNATTENDED;
        return NONE;
    }
    void accepted() { forwarded_ = true; }
    uint32_t event() const { return active_ ? id_ : 0; }
    bool acknowledged() const { return active_ && ack_; }
    bool forwarded() const { return active_ && forwarded_; }
};
} // namespace emergency_policy
