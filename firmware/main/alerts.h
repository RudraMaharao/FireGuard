#pragma once
#include "sensors.h"
#include <stdint.h>
namespace alerts {
struct Health {
    uint32_t sent = 0, failed = 0, dropped = 0;
    int httpStatus=0, networkError=0;
    int registered = 0;
    bool internet = false;
    uint32_t event = 0;
    bool acknowledged = false, forwarded = false;
};
void start();
void observe(const sensors::Snapshot &s); // non-blocking queue operation only
bool test();
bool readings();
bool snapshot();
bool test_emergency();
bool acknowledge(uint32_t event);
Health health();
} // namespace alerts
