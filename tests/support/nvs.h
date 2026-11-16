#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>
#include <map>
#include <string>
using nvs_handle_t=unsigned;
using esp_err_t=int;
constexpr int ESP_OK=0,NVS_READWRITE=1,NVS_READONLY=0;
constexpr int ESP_ERR_NVS_NOT_FOUND=-1, ESP_ERR_NVS_NOT_ENOUGH_SPACE=-2, ESP_ERR_NVS_NOT_INITIALIZED=-3;
inline const char *esp_err_to_name(esp_err_t e){return e==ESP_OK?"ESP_OK":e==-2?"ESP_ERR_NVS_NOT_ENOUGH_SPACE":e==-3?"ESP_ERR_NVS_NOT_INITIALIZED":"ESP_ERR_NVS_NOT_FOUND";}
inline std::map<std::string,std::vector<uint8_t>> nvs_data;
inline std::map<nvs_handle_t,std::string> nvs_handles;
inline unsigned nvs_next_handle=1;
inline std::string nvs_full_partition;
inline bool nvs_fail_commit=false;
inline esp_err_t nvs_partition_init_result=ESP_OK;
inline esp_err_t nvs_open_from_partition(const char*p,const char*ns,int,nvs_handle_t*h){
 if(std::string(p)=="fg_config" && nvs_partition_init_result!=ESP_OK)return nvs_partition_init_result;
 *h=nvs_next_handle++;nvs_handles[*h]=std::string(p)+"/"+ns+"/";return ESP_OK;
}
inline esp_err_t nvs_open(const char*ns,int mode,nvs_handle_t*h){return nvs_open_from_partition("nvs",ns,mode,h);}
inline void nvs_close(nvs_handle_t h){nvs_handles.erase(h);}
inline esp_err_t nvs_commit(nvs_handle_t){return nvs_fail_commit?ESP_ERR_NVS_NOT_ENOUGH_SPACE:ESP_OK;}
inline esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*v,size_t n){
 if(!nvs_full_partition.empty() && nvs_handles[h].rfind(nvs_full_partition+"/",0)==0)return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
 auto*b=(const uint8_t*)v;nvs_data[nvs_handles[h]+k]=std::vector<uint8_t>(b,b+n);return ESP_OK;
}
inline esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*v,size_t*n){auto i=nvs_data.find(nvs_handles[h]+k);if(i==nvs_data.end())return ESP_ERR_NVS_NOT_FOUND;size_t size=i->second.size();if(!v){*n=size;return ESP_OK;}if(*n<size){*n=size;return -1;}memcpy(v,i->second.data(),size);*n=size;return ESP_OK;}
