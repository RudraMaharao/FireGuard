#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,I2C_ADDR_BIT_LEN_7=0;
using i2c_master_bus_handle_t=void*;
using i2c_master_dev_handle_t=void*;
struct i2c_device_config_t {int dev_addr_length;int device_address;int scl_speed_hz;};
inline uint8_t registers[256]={};inline bool i2c_fail=false;inline int present_addr=0x77;
inline bool ads_mode=false, ads_busy=false;
inline unsigned ads_mux=0, ads_reads=0;
inline uint16_t ads_config=0, ads_codes[4]={8000,16000,20000,0};
inline esp_err_t i2c_master_transmit_receive(void*,const uint8_t*r,size_t,uint8_t*out,size_t n,int){if(i2c_fail)return ESP_FAIL;
if(ads_mode){uint16_t v=*r==1?(ads_busy?ads_config&0x7fff:ads_config|0x8000):ads_codes[ads_mux];out[0]=v>>8;out[1]=v&255;++ads_reads;return ESP_OK;}
memcpy(out,registers+*r,n);return ESP_OK;}
inline esp_err_t i2c_master_transmit(void*,const uint8_t*b,size_t n,int){if(i2c_fail)return ESP_FAIL;
if(ads_mode){if(n!=3||b[0]!=1)return ESP_FAIL;ads_config=(b[1]<<8)|b[2];ads_mux=((ads_config>>12)&7)-4;return ESP_OK;}
registers[b[0]]=b[1];return ESP_OK;}
inline esp_err_t i2c_master_bus_rm_device(void*){return ESP_OK;}
inline esp_err_t i2c_master_probe(void*,int addr,int){return addr==present_addr?ESP_OK:ESP_FAIL;}
inline esp_err_t i2c_master_bus_add_device(void*,const i2c_device_config_t*,void**dev){*dev=(void*)1;return ESP_OK;}
