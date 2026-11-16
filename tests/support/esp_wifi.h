#pragma once
// Wi-Fi scan stand-in for host tests: tests fill fake_aps.
#include "nvs.h"
#include <stdint.h>
#include <string.h>
#include <vector>
struct wifi_ap_record_t{uint8_t bssid[6]={};uint8_t ssid[33]={};uint8_t primary=0;int8_t rssi=0;};
constexpr int WIFI_SCAN_TYPE_PASSIVE=1;
struct wifi_scan_config_t{int scan_type=0;struct {unsigned passive=0;} scan_time;const uint8_t*ssid=nullptr;const uint8_t*bssid=nullptr;uint8_t channel=0;bool show_hidden=false;};
inline std::vector<wifi_ap_record_t> fake_aps;inline int fake_scans=0;inline esp_err_t fake_scan_result=ESP_OK;
inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t*,bool){fake_scans++;return fake_scan_result;}
inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t*n,wifi_ap_record_t*r){uint16_t c=0;for(auto&a:fake_aps){if(c>=*n)break;r[c++]=a;}*n=c;return ESP_OK;}
inline wifi_ap_record_t fake_ap(const char*ssid,uint8_t last,int8_t rssi,uint8_t ch){wifi_ap_record_t a;uint8_t b[6]={0xa4,0x2b,0xb0,0x10,0x20,last};memcpy(a.bssid,b,6);strncpy((char*)a.ssid,ssid,32);a.rssi=rssi;a.primary=ch;return a;}

inline esp_err_t esp_wifi_scan_stop(){return ESP_OK;}
