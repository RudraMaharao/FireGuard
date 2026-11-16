#pragma once
#include "driver/i2c_master.h"
#include <math.h>
class Ads1115 {
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t dev = nullptr;
    uint32_t retryAt = 0;
    bool readReg(uint8_t reg, uint16_t &value);
    void disconnect();
  public:
    bool begin(i2c_master_bus_handle_t bus);
    // Single-ended input voltage, before undoing the external 1:2 divider.
    bool readMv(unsigned channel, float &mv);
};
