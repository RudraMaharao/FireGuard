#pragma once
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
template <class T> constexpr T min(T a, T b) { return a < b ? a : b; }
template <class T> constexpr T max(T a, T b) { return a > b ? a : b; }
template <class T> constexpr T constrain(T v, T lo, T hi) { return min(max(v, lo), hi); }
inline uint32_t millis() { return uint32_t(esp_timer_get_time() / 1000); }
inline void *ps_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#define F(x) x
constexpr int ADC_11db = 3;
void analogSetPinAttenuation(int pin, int);
uint32_t analogReadMilliVolts(int pin);
struct Stream {
    template <class... A> int printf(const char *fmt, A... a) { return ::printf(fmt, a...); }
    void println(const char *s = "") { ::puts(s); }
};
inline Stream Serial;
