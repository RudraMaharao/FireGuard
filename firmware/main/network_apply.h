#pragma once
#include <atomic>
#include <stdint.h>
// Shared by the HTTP server and network worker. No sleeping in the HTTP handler.
class NetworkApply {
    std::atomic<uint32_t> deadline{0};
public:
    void schedule(uint32_t now, uint32_t delayMs) {
        uint32_t when = now + delayMs;
        deadline.store(when ? when : 1);
    }
    bool pending() const { return deadline.load() != 0; }
    bool take(uint32_t now) {
        uint32_t when = deadline.load();
        return when && int32_t(now - when) >= 0 && deadline.compare_exchange_strong(when, 0);
    }
};
