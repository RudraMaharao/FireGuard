#include "vision_service.h"
#include "camera.h"
#include "detector.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "sensors.h"
#include "storage.h"
#include "vision_mapping.h"
#include <atomic>
namespace vision_service {
namespace {
SemaphoreHandle_t lock;
Evidence last;
std::vector<uint8_t> jpeg;
std::atomic<bool> running{false}, save{false}, manual{false}, ready{false};
std::atomic<uint32_t> imageAt{0}, attemptedAt{0};
std::atomic<int> captureError{0};
void task(void *) {
    uint8_t *rgb = (uint8_t *)ps_malloc(camera::BYTES), *small = (uint8_t *)ps_malloc(VN * 2);
    Vision vis;
    VisionSmoother smoother;
    if (!rgb || !small || !vis.begin() || !detector::load()) {
        captureError=ESP_FAIL;
        ESP_LOGE("vision", "vision unavailable");
        vTaskDelete(nullptr);
        return;
    }
    ready=true;
    uint32_t prev = 0;
    bool was = false;
    Verdict verdict;
    bool haveFrame = false;
    uint32_t frameAt = 0;
    for (;;) {
        bool requested = manual.exchange(false);
        bool wanted = requested || sensors::snapshot().cameraRequested;
        running = wanted;
        if (wanted) {
            if (!was) {
                smoother = VisionSmoother();
                vis.frames = 0;
                vis.lastHash = 0;
                vis.edgeRef = -1;
                vis.stillFor = 0;
                memset(vis.lumaPrev, 0, VN);
                memset(vis.prevMask, 0, VN);
                prev = 0;
            }
            uint32_t started = millis();
            attemptedAt=millis();
            std::vector<uint8_t> captured;
            int result=camera::take_photo(rgb, &captured);
            captureError=result;
            if (result == ESP_OK) {
                uint32_t now = millis();
                vis.dtFrame = prev ? (now - prev) / 1000.0f : 0.5f;
                prev = now;
                // Camera produces BE words; the classical analyser reads native uint16_t.
                auto *native = (uint16_t *)small;
                for (int y = 0; y < VH; y++)
                    for (int x = 0; x < VW; x++) {
                        int k = ((y * 2) * camera::WIDTH + x * 2) * 2;
                        native[y * VW + x] = (uint16_t(rgb[k]) << 8) | rgb[k + 1];
                    }
                VisionOut classical = vis.analyse(small);
                detector::run(rgb, camera::WIDTH, camera::HEIGHT, verdict);
                Evidence e{smoother.push(map_yolo(classical, verdict, camera::WIDTH, camera::HEIGHT)),
                           millis()};
                xSemaphoreTake(lock, portMAX_DELAY);
                last = e;
                jpeg = std::move(captured);
                imageAt=e.at;
                xSemaphoreGive(lock);
                haveFrame = true;
                frameAt = millis();
            } else {
                xSemaphoreTake(lock, portMAX_DELAY);
                last = {};
                jpeg.clear();
                imageAt=0;
                xSemaphoreGive(lock);
                haveFrame = false;
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            (void)started;
        }
        was = wanted;
        if (save.exchange(false) && haveFrame && millis() - frameAt <= 4000)
            storage::save(rgb, camera::WIDTH, camera::HEIGHT, verdict, storage::FALSE_ALARM, 0);
        vTaskDelay(pdMS_TO_TICKS(wanted ? 100 : 200));
    }
}
} // namespace
void start() {
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
    configASSERT(xTaskCreatePinnedToCore(task, "vision", 12288, nullptr, 3, nullptr, 1) == pdPASS);
}
Evidence evidence() {
    Evidence e;
    if (!lock)
        return e;
    xSemaphoreTake(lock, portMAX_DELAY);
    e = last;
    xSemaphoreGive(lock);
    return e;
}
bool active() { return running.load(); }
bool save_sample() {
    auto e = evidence();
    if (!running || !e.at || millis() - e.at > 4000)
        return false;
    save = true;
    return true;
}
bool request_capture() {
    if (!lock) return false;
    manual=true;
    return true;
}
Health health() {
    int error=captureError.load();
    return {ready.load(),running.load()||manual.load(),imageAt.load(),attemptedAt.load(),error,
            error ? (ready ? camera::last_error_text() : "Vision/model initialization failed") : ""};
}
bool photo(std::vector<uint8_t> &out, uint32_t maxAgeMs) {
    if (!lock)
        return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = last.at && millis() - last.at <= maxAgeMs && !jpeg.empty();
    if (ok)
        out = jpeg;
    xSemaphoreGive(lock);
    return ok;
}
} // namespace vision_service
