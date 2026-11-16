#pragma once
#include "driver/i2c_master.h"
#include <stdint.h>
class Bme280 {
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t dev = nullptr;
    uint16_t t1 = 0, p1 = 0;
    int16_t t2 = 0, t3 = 0, p[8] = {}, h2 = 0, h4 = 0, h5 = 0;
    uint8_t h1 = 0, h3 = 0;
    int8_t h6 = 0;
    uint32_t retry = 0;
    bool reg(uint8_t r, uint8_t *out, size_t n);
    bool write(uint8_t r, uint8_t v);

  public:
    bool begin(i2c_master_bus_handle_t b);
    bool read(float &t, float &rh, float &hpa);
};
