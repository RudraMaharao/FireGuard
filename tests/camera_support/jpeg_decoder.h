#pragma once
#include "esp_camera.h"
#include <assert.h>
constexpr int JPEG_IMAGE_FORMAT_RGB565=1,JPEG_IMAGE_SCALE_0=0;
struct esp_jpeg_image_cfg_t {uint8_t*indata;size_t indata_size;uint8_t*outbuf;size_t outbuf_size;int out_format,out_scale;struct{unsigned swap_color_bytes;}flags;};
struct esp_jpeg_image_output_t {unsigned width,height;size_t output_len;};
inline bool fake_decode_fail=false;
inline int esp_jpeg_decode(esp_jpeg_image_cfg_t*c,esp_jpeg_image_output_t*out){
 assert(!fake_camera_active); // DMA must be freed before the decoder allocates workspace
 assert(c->outbuf_size==320*240*2 && c->out_format==JPEG_IMAGE_FORMAT_RGB565 && c->flags.swap_color_bytes==1);
 if(fake_decode_fail)return ESP_FAIL;
 c->outbuf[0]=0xf8;c->outbuf[1]=0x00;*out={320,240,320*240*2};return ESP_OK;
}
