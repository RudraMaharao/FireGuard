#include "camera.h"
#include "esp_camera.h"
#include "jpeg_decoder.h"
#include <assert.h>
#include <iostream>
#include <vector>
int main(){
 std::vector<uint8_t>rgb(camera::BYTES),jpeg;
 assert(camera::take_photo(rgb.data(),&jpeg)==ESP_OK);
 assert(fake_config.pixel_format==PIXFORMAT_JPEG && fake_config.fb_count==1 && fake_config.pin_sccb_sda==4 && fake_config.pin_sccb_scl==5);
 assert(rgb[0]==0xf8 && rgb[1]==0x00 && jpeg.size()==sizeof fake_jpeg && fake_returns==7 && !fake_camera_active);
 fake_bad_frame=true;assert(camera::take_photo(rgb.data(),&jpeg)!=ESP_OK && jpeg.empty() && !fake_camera_active);fake_bad_frame=false;
 fake_no_frame=true;assert(camera::take_photo(rgb.data(),&jpeg)!=ESP_OK && jpeg.empty() && !fake_camera_active);fake_no_frame=false;
 fake_init_fail=true;assert(camera::take_photo(rgb.data(),&jpeg)!=ESP_OK && !fake_camera_active);fake_init_fail=false;
 fake_decode_fail=true;assert(camera::take_photo(rgb.data(),&jpeg)!=ESP_OK && jpeg.empty() && !fake_camera_active);
 assert(camera::take_photo(nullptr,&jpeg)!=ESP_OK);
 std::cout<<"JPEG capture, one buffer, bounded RGB565BE decode, release-before-decode, failure cleanup and no stale JPEG passed\n";
}
