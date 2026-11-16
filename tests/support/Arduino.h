#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
template<class T> constexpr T min(T a,T b){return a<b?a:b;}
template<class T> constexpr T max(T a,T b){return a>b?a:b;}
template<class T> constexpr T constrain(T a,T b,T c){return min(max(a,b),c);}
inline uint32_t millis(){return 1;}
inline void*ps_malloc(size_t n){return malloc(n);}
#define F(x) x
struct Stream {template<class...A>int printf(const char*f,A...a){return ::printf(f,a...);}void println(const char*s){puts(s);}};
constexpr int ADC_11db=3;
inline void analogSetPinAttenuation(int,int){}
inline uint32_t test_mv=1000;
inline uint32_t analogReadMilliVolts(int){return test_mv;}
