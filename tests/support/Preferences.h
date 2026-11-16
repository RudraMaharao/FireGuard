#pragma once
#include <stdint.h>
struct Preferences {
 bool begin(const char*,bool=false){return true;}void end(){}
 uint8_t getUChar(const char*,uint8_t d=0){return d;}
 uint32_t getULong(const char*,uint32_t d=0){return d;}
 float getFloat(const char*,float d=0){return d;}
 void putUChar(const char*,uint8_t){}void putULong(const char*,uint32_t){}void putFloat(const char*,float){}
};
