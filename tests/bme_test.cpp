#include "bme280.h"
#include "esp_timer.h"
#include <assert.h>
#include <iostream>
#include <math.h>
void put16(int a, int v) {
    registers[a] = v & 255;
    registers[a + 1] = (v >> 8) & 255;
}
void put20(int a, int v) {
    registers[a] = v >> 12;
    registers[a + 1] = (v >> 4) & 255;
    registers[a + 2] = (v & 15) << 4;
}
int main() {
    registers[0xd0] = 0x60;
    put16(0x88, 27504);
    put16(0x8a, 26435);
    put16(0x8c, -1000);
    int cal[] = {36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000};
    for (int i = 0; i < 9; i++)
        put16(0x8e + i * 2, cal[i]);
    registers[0xa1] = 75;
    put16(0xe1, 362);
    registers[0xe3] = 0;
    int h4 = 325, h5 = 50;
    registers[0xe4] = h4 >> 4;
    registers[0xe5] = (h5 & 15) << 4 | (h4 & 15);
    registers[0xe6] = h5 >> 4;
    registers[0xe7] = 30;
    put20(0xf7, 415148);
    put20(0xfa, 519888);
    registers[0xfd] = 0x75;
    registers[0xfe] = 0x30;
    Bme280 b;
    assert(b.begin((void *)1)); // probes fallback 0x77
    assert(registers[0xf2] == 4 && registers[0xf4] == 0x57 && registers[0xf5] == 0x88);
    float t, h, p;
    assert(b.read(t, h, p));
    assert(fabs(t - 25.08248) < 0.001);
    assert(fabs(p - 1006.53267) < 0.001);
    assert(h >= 0 && h <= 100);
    put20(0xfa, 0x80000);
    assert(!b.read(t, h, p) && isnan(t) && isnan(h) && isnan(p));
    put20(0xfa, 519888);
    i2c_fail = true;
    assert(!b.read(t, h, p));
    i2c_fail = false;
    fake_time_us = 31000000;
    assert(!b.read(t, h, p));
    assert(b.read(t, h, p)); // recovery reinitializes first
    registers[0xd0] = 0x58;
    Bme280 bmp;
    assert(!bmp.begin((void *)1));
    std::cout << "BME280 compensation, register setup, fault and recovery tests passed\n";
}
