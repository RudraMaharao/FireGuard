#include "ads1115.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace { uint32_t nowMs() { return uint32_t(esp_timer_get_time()/1000); } }
void Ads1115::disconnect() {
    if (dev) i2c_master_bus_rm_device(dev);
    dev = nullptr; retryAt = nowMs();
}
bool Ads1115::readReg(uint8_t r, uint16_t &value) {
    uint8_t b[2];
    if (!dev || i2c_master_transmit_receive(dev, &r, 1, b, 2, 30) != ESP_OK) return false;
    value = (uint16_t(b[0]) << 8) | b[1]; return true;
}
bool Ads1115::begin(i2c_master_bus_handle_t b) {
    bus = b; disconnect();
    if (!bus || i2c_master_probe(bus, 0x48, 30) != ESP_OK) return false;
    i2c_device_config_t c = {};
    c.dev_addr_length = I2C_ADDR_BIT_LEN_7; c.device_address = 0x48; c.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &c, &dev) == ESP_OK;
}
bool Ads1115::readMv(unsigned ch, float &mv) {
    mv = NAN;
    if (ch > 3) return false;
    if (!dev && (nowMs()-retryAt < 5000 || !begin(bus))) return false;
    float total = 0;
    for (int sample = 0; sample < 4; ++sample) {
        // OS=start, MUX=AINx/GND, PGA=+/-4.096 V, single-shot, 128 SPS, comparator disabled.
        uint16_t cfg = 0x8383 | ((4u+ch) << 12);
        uint8_t command[] = {1, uint8_t(cfg >> 8), uint8_t(cfg)};
        if (i2c_master_transmit(dev, command, sizeof command, 30) != ESP_OK) { disconnect(); return false; }
        uint16_t ready = 0;
        bool complete = false;
        // Bound the wait even if the bus responds but the ADC never completes.
        for (int n = 0; n < 15; ++n) {
            const TickType_t delay = pdMS_TO_TICKS(2);
            vTaskDelay(delay ? delay : 1);
            if (!readReg(1, ready)) { disconnect(); return false; }
            if (ready & 0x8000) { complete = true; break; }
        }
        uint16_t raw = 0;
        if (!complete || !readReg(0, raw)) { disconnect(); return false; }
        // ADS1115 conversion register is signed, MSB first; 125 uV/LSB at this gain.
        float volts = int16_t(raw) * 0.125f;
        if (volts < 0 || volts > 3250) return false;
        total += volts;
    }
    mv = total / 4; return true;
}
