#include "bme280.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <initializer_list>
#include <math.h>
namespace {
uint16_t u16(const uint8_t *b) { return uint16_t(b[0]) | (uint16_t(b[1]) << 8); }
int16_t s16(const uint8_t *b) { return int16_t(u16(b)); }
int16_t s12(int v) { return v & 0x800 ? v - 4096 : v; }
} // namespace
bool Bme280::reg(uint8_t r, uint8_t *out, size_t n) {
    return dev && i2c_master_transmit_receive(dev, &r, 1, out, n, 50) == ESP_OK;
}
bool Bme280::write(uint8_t r, uint8_t v) {
    uint8_t b[] = {r, v};
    return dev && i2c_master_transmit(dev, b, 2, 50) == ESP_OK;
}
bool Bme280::begin(i2c_master_bus_handle_t b) {
    bus = b;
    retry = uint32_t(esp_timer_get_time() / 1000);
    if (!bus)
        return false;
    if (dev) {
        i2c_master_bus_rm_device(dev);
        dev = nullptr;
    }
    for (int addr : {0x76, 0x77}) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK)
            continue;
        i2c_device_config_t c = {};
        c.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        c.device_address = addr;
        c.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus, &c, &dev) != ESP_OK)
            continue;
        uint8_t id = 0;
        if (reg(0xd0, &id, 1) && id == 0x60)
            break;
        i2c_master_bus_rm_device(dev);
        dev = nullptr;
    }
    if (!dev)
        return false;
    // Reset and wait until the calibration image has copied from NVM.
    if (!write(0xe0, 0xb6))
        return false;
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t status = 1;
    for (int i = 0; i < 20; ++i) {
        if (!reg(0xf3, &status, 1))
            return false;
        if (!(status & 1))
            break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (status & 1)
        return false;
    uint8_t a[26], h[7];
    if (!reg(0x88, a, sizeof a) || !reg(0xe1, h, sizeof h))
        return false;
    t1 = u16(a);
    t2 = s16(a + 2);
    t3 = s16(a + 4);
    p1 = u16(a + 6);
    for (int i = 0; i < 8; ++i)
        p[i] = s16(a + 8 + i * 2);
    h1 = a[25];
    h2 = s16(h);
    h3 = h[2];
    h4 = s12((h[3] << 4) | (h[4] & 15));
    h5 = s12((h[5] << 4) | (h[4] >> 4));
    h6 = int8_t(h[6]);
    if (!t1 || !p1)
        return false;
    // Humidity x8, temperature x2, pressure x16; filter x4, standby 500 ms.
    return write(0xf2, 4) && write(0xf5, (4 << 5) | (2 << 2)) && write(0xf4, (2 << 5) | (5 << 2) | 3);
}
bool Bme280::read(float &t, float &rh, float &hpa) {
    t = rh = hpa = NAN;
    if (!dev) {
        if (uint32_t(esp_timer_get_time() / 1000) - retry >= 30000)
            begin(bus);
        return false;
    }
    uint8_t b[8];
    if (!reg(0xf7, b, sizeof b)) {
        i2c_master_bus_rm_device(dev);
        dev = nullptr;
        retry = uint32_t(esp_timer_get_time() / 1000);
        return false;
    }
    int ap = (b[0] << 12) | (b[1] << 4) | (b[2] >> 4), at = (b[3] << 12) | (b[4] << 4) | (b[5] >> 4),
        ah = (b[6] << 8) | b[7];
    if (ap == 0x80000 || at == 0x80000 || ah == 0x8000 || !p1)
        return false;
    // Bosch datasheet section 8.1: floating-point compensation, pressure in Pa.
    double v1 = (at / 16384.0 - t1 / 1024.0) * t2;
    double v2 = (at / 131072.0 - t1 / 8192.0);
    v2 = v2 * v2 * t3;
    double fine = v1 + v2, tc = fine / 5120.0;
    v1 = fine / 2.0 - 64000.0;
    v2 = v1 * v1 * p[4] / 32768.0 + v1 * p[3] * 2.0;
    v2 = v2 / 4.0 + p[2] * 65536.0;
    v1 = (p[1] * v1 * v1 / 524288.0 + p[0] * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * p1;
    if (v1 == 0)
        return false;
    double pressure = (1048576.0 - ap - v2 / 4096.0) * 6250.0 / v1;
    v1 = p[7] * pressure * pressure / 2147483648.0;
    v2 = pressure * p[6] / 32768.0;
    pressure += (v1 + v2 + p[5]) / 16.0;
    double hum = fine - 76800.0;
    hum = (ah - (h4 * 64.0 + h5 / 16384.0 * hum)) *
          (h2 / 65536.0 * (1.0 + h6 / 67108864.0 * hum * (1.0 + h3 / 67108864.0 * hum)));
    hum *= 1.0 - h1 * hum / 524288.0;
    hum = fmax(0.0, fmin(100.0, hum));
    if (!isfinite(tc) || !isfinite(pressure) || !isfinite(hum) || tc < -40 || tc > 85 || pressure < 30000 ||
        pressure > 110000)
        return false;
    t = tc;
    rh = hum;
    hpa = pressure / 100.0;
    return true;
}
