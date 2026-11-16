#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
constexpr int PIXFORMAT_JPEG=1,PIXFORMAT_RGB565=2,FRAMESIZE_QVGA=1;
constexpr int LEDC_TIMER_1=1,LEDC_CHANNEL_1=1,CAMERA_FB_IN_PSRAM=1,CAMERA_GRAB_WHEN_EMPTY=1;
struct camera_config_t {int pin_pwdn,pin_reset,pin_xclk,pin_sccb_sda,pin_sccb_scl,pin_d7,pin_d6,pin_d5,pin_d4,pin_d3,pin_d2,pin_d1,pin_d0,pin_vsync,pin_href,pin_pclk,xclk_freq_hz,ledc_timer,ledc_channel,pixel_format,frame_size,jpeg_quality,fb_count,fb_location,grab_mode,sccb_i2c_port;};
struct sensor_t { int(*set_vflip)(sensor_t*,int);int(*set_hmirror)(sensor_t*,int); };
struct camera_fb_t {uint8_t*buf;size_t len;int width,height,format;};
inline bool fake_camera_active=false,fake_init_fail=false,fake_no_frame=false,fake_bad_frame=false;
inline int fake_returns=0,fake_deinits=0;
inline camera_config_t fake_config;
inline uint8_t fake_jpeg[]={0xff,0xd8,1,2,0xff,0xd9};
inline camera_fb_t fake_fb{fake_jpeg,sizeof(fake_jpeg),320,240,PIXFORMAT_JPEG};
inline int esp_camera_init(camera_config_t*c){fake_config=*c;if(fake_init_fail)return ESP_FAIL;fake_camera_active=true;return ESP_OK;}
inline int esp_camera_deinit(){fake_camera_active=false;fake_deinits++;return ESP_OK;}
inline sensor_t *esp_camera_sensor_get(){return nullptr;}
inline camera_fb_t *esp_camera_fb_get(){fake_fb.width=fake_bad_frame?640:320;return fake_no_frame?nullptr:&fake_fb;}
inline void esp_camera_fb_return(camera_fb_t*){fake_returns++;}
