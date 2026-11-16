#include "camera.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_log.h"
#include <string.h>
#include "jpeg_decoder.h"
#include <atomic>
#include <vector>

namespace {
const char *TAG = "camera";
std::atomic<const char *> failure{"Not requested"};

void power_down_pin(bool off)
{
    if constexpr (CAM_PIN_PWDN >= 0) {
        gpio_set_level((gpio_num_t)CAM_PIN_PWDN, off ? 1 : 0);  // PWDN high = sensor powered down
    }
}
}  // namespace

namespace camera {

void init_power_pin()
{
    if constexpr (CAM_PIN_PWDN >= 0) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << CAM_PIN_PWDN;
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        power_down_pin(true);
    }
}

const char *last_error_text() { return failure.load(); }

esp_err_t take_photo(uint8_t *dst, std::vector<uint8_t> *jpeg)
{
    if (jpeg) jpeg->clear();
    if (!dst) { failure="Missing RGB output buffer"; return ESP_FAIL; }
    camera_config_t cfg = {};
    cfg.pin_pwdn = CAM_PIN_PWDN;
    cfg.pin_reset = CAM_PIN_RESET;
    cfg.pin_xclk = CAM_PIN_XCLK;
    cfg.pin_sccb_sda = CAM_PIN_SIOD;
    cfg.pin_sccb_scl = CAM_PIN_SIOC;
    cfg.pin_d7 = CAM_PIN_D7;
    cfg.pin_d6 = CAM_PIN_D6;
    cfg.pin_d5 = CAM_PIN_D5;
    cfg.pin_d4 = CAM_PIN_D4;
    cfg.pin_d3 = CAM_PIN_D3;
    cfg.pin_d2 = CAM_PIN_D2;
    cfg.pin_d1 = CAM_PIN_D1;
    cfg.pin_d0 = CAM_PIN_D0;
    cfg.pin_vsync = CAM_PIN_VSYNC;
    cfg.pin_href = CAM_PIN_HREF;
    cfg.pin_pclk = CAM_PIN_PCLK;
    cfg.xclk_freq_hz = CAM_XCLK_HZ;
    cfg.ledc_timer = LEDC_TIMER_1;      // timer 0 is left for the buzzer
    cfg.ledc_channel = LEDC_CHANNEL_1;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = FRAMESIZE_QVGA;    // 320x240, matches the 320 model input
    cfg.jpeg_quality = 12;
    cfg.fb_count = 1;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY; // one compressed frame; avoids raw RGB double buffering
    cfg.sccb_i2c_port = 1;              // ADS1115/BME280 use I2C port 0

    failure = "Camera initialization failed";
    power_down_pin(false);
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        esp_camera_deinit();
        power_down_pin(true);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, CAM_VFLIP);
        s->set_hmirror(s, CAM_HMIRROR);
    }

    // Let auto-exposure and white balance settle: the first frames after power-up are dark/green.
    for (int i = 0; i < CAM_WARMUP_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }

    err = ESP_FAIL;
    std::vector<uint8_t> compressed;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        failure = "Camera returned no frame";
        ESP_LOGE(TAG, "no frame from camera");
    } else {
        if (fb->format != PIXFORMAT_JPEG || fb->width != WIDTH || fb->height != HEIGHT || fb->len < 4) {
            failure = "Unexpected JPEG frame dimensions/format";
            ESP_LOGE(TAG, "unexpected frame %dx%d fmt %d len %d", fb->width, fb->height, fb->format, (int)fb->len);
        } else compressed.assign(fb->buf, fb->buf + fb->len);
        esp_camera_fb_return(fb);
    }
    // Release camera DMA/internal buffers BEFORE decoding or starting HTTPS uploads.
    esp_camera_deinit();
    power_down_pin(true);
    if (compressed.empty()) return ESP_FAIL;
    esp_jpeg_image_cfg_t decode = {};
    decode.indata = compressed.data();
    decode.indata_size = compressed.size();
    decode.outbuf = dst;
    decode.outbuf_size = BYTES;
    decode.out_format = JPEG_IMAGE_FORMAT_RGB565;
    decode.out_scale = JPEG_IMAGE_SCALE_0;
    decode.flags.swap_color_bytes = 1; // RGB565BE: preserves the CNN/classical preprocessing contract
    esp_jpeg_image_output_t decoded = {};
    err = esp_jpeg_decode(&decode, &decoded);
    if (err != ESP_OK || decoded.width != WIDTH || decoded.height != HEIGHT || decoded.output_len != BYTES) {
        failure = "JPEG decode failed";
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (jpeg) *jpeg = std::move(compressed);
    failure = "";
    return ESP_OK;
}

} // namespace camera
