#include "alerts.h"
#include "board_i2c.h"
#include "buzzer.h"
#include "camera.h"
#include "cloud_link.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "geolocate.h"
#include "nvs_flash.h"
#include "sensors.h"
#include "settings.h"
#include "sms.h"
#include "storage.h"
#include "vision_service.h"
#include "wifi_sync.h"
#include <fcntl.h>
#include <unistd.h>
namespace {
void allocation_failed(size_t bytes, uint32_t caps, const char *caller) {
    // ROM output does not allocate: safe even when a libc mutex allocation fails.
    esp_rom_printf("FireGuard allocation failed: bytes=%u caps=0x%x caller=%s\n",
                   unsigned(bytes), unsigned(caps), caller ? caller : "unknown");
}
struct Button {
    int pin;
    bool raw = false, stable = false, fired = false;
    uint32_t changed = 0, down = 0;
    // 30 ms debounce, with one long-press action per physical press.
    int poll(uint32_t now, uint32_t longMs) {
        bool v = gpio_get_level(gpio_num_t(pin)) == 0;
        if (v != raw) {
            raw = v;
            changed = now;
        }
        if (v != stable && now - changed >= 30) {
            stable = v;
            if (v) {
                down = now;
                fired = false;
            } else if (!fired)
                return 1;
        }
        if (stable && !fired && now - down >= longMs) {
            fired = true;
            return 2;
        }
        return 0;
    }
};
} // namespace
extern "C" void app_main() {
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
    // This SDK lazily allocates its realtime/adjtime mutex on the first time() call.
    // Reserve it before Wi-Fi, NVS caches and worker stacks consume internal RAM.
    (void)time(nullptr);
    ESP_LOGI("memory", "libc clock initialized before application workers");
    ESP_ERROR_CHECK(nvs_flash_init());
    settings::init();
    sms::init();
    geolocate::init();
    ESP_ERROR_CHECK(board_i2c::init());
    camera::init_power_pin();
    buzzer::init();
    storage::init();
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_BUTTON) | (1ULL << PIN_BTN_RIGHT) | (1ULL << PIN_BTN_WRONG);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI("memory","before Wi-Fi: internal free %u, largest %u; PSRAM free %u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    auto wifiError=wifi_sync::init();
    if(wifiError!=ESP_OK)ESP_LOGE("network","Wi-Fi init failed (%s); continuing local sensing",esp_err_to_name(wifiError));
    ESP_LOGI("memory","after radio start: internal free %u, largest %u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    sensors::start(board_i2c::bus());
    sms::start();
    alerts::start();
    vision_service::start();
    wifi_sync::start();
    geolocate::start(); // waits for Wi-Fi + clock, then looks up and caches the Wi-Fi location
    cloud_link::start();
    ESP_LOGI("memory","all workers started: internal free %u, largest %u; PSRAM free %u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    Button b1{PIN_BUTTON}, b2{PIN_BTN_RIGHT}, b3{PIN_BTN_WRONG};

    bool labelCommand = false;
    uint32_t memoryAt=0;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    for (;;) {
        uint32_t now = millis();
        if(now-memoryAt>=30000){
            memoryAt=now;
            ESP_LOGI("memory","running: internal free %u, largest %u; PSRAM free %u; main stack spare %u",
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
        }
        int v = b1.poll(now, 1500);
        if (v == 1)
            sensors::command(sensors::MUTE);
        if (v == 2)
            sensors::command(sensors::CYCLE_LABEL);
        v = b2.poll(now, 5000);
        if (v == 1) {
            alerts::readings();
        }
        if (v == 2)
            wifi_sync::setup();
        v = b3.poll(now, 1500);
        if (v == 1 && !vision_service::save_sample())
            ESP_LOGI("buttons", "No fresh camera frame to save");
        if (v == 2)
            alerts::test();
        char c;
        while (read(STDIN_FILENO, &c, 1) > 0) {
            if (labelCommand) {
                if (c >= '0' && c <= '9') {
                    auto s = sensors::snapshot();
                    int n = (c - '0' + 10 - s.label) % 10;
                    for (int i = 0; i < n; i++)
                        sensors::command(sensors::CYCLE_LABEL);
                }
                labelCommand = false;
                continue;
            }
            switch (c) {
            case 'm':
                sensors::command(sensors::MUTE);
                break;
            case 'L':
                labelCommand = true;
                break;
            case 'C':
                sensors::command(sensors::CAPTURE_R0);
                break;
            case 's':
                sensors::command(sensors::PRINT_STATUS);
                break;
            case 'p':
                sensors::command(sensors::PRINT_PHYSICS);
                break;
            case 'a':
                sensors::command(sensors::PRINT_ADAPT);
                break;
            case 'A':
                sensors::command(sensors::RESET_ADAPT);
                break;
            case 'x':
                sensors::command(sensors::PRINT_CROSSVAL);
                break;
            case 'B':
                sensors::command(sensors::PRINT_BURNIN);
                break;
            }
        }
        auto s = sensors::snapshot();
        int severity = severityOf(SysState(s.state));
        uint32_t period = severity >= 4 ? 500 : severity >= 2 ? 1200 : 3000;
        buzzer::set(!s.muted && severity > 0 && (now % period < (severity >= 4 ? 250UL : 100UL)));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
