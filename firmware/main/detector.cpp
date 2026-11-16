#include "detector.h"
#include "config.h"
#include "dl_detect_base.hpp"
#include "dl_detect_yolo11_postprocessor.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

// main/models/fire_det.espdl, embedded by main/CMakeLists.txt
extern const uint8_t fire_det_espdl[] asm("_binary_fire_det_espdl_start");

namespace {
const char *TAG = "detector";

class FireModel : public dl::detect::DetectImpl {
public:
    FireModel(float score_thr, float nms_thr)
    {
        // Copy the weights into PSRAM when there is room: faster than reading them from flash.
        bool param_copy = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >= 4 * 1024 * 1024;
        m_model = new dl::Model((const char *)fire_det_espdl, fbs::MODEL_LOCATION_IN_FLASH_RODATA, 0,
                                dl::MEMORY_MANAGER_GREEDY, nullptr, param_copy);
        m_model->minimize();
        // Same preprocessing as training/calibration: pixels / 255, letterboxed with grey 114.
        m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
        m_image_preprocessor->enable_letterbox({114, 114, 114});
        // YOLO11 heads at strides 8/16/32: {stride_y, stride_x, offset_y, offset_x}
        m_postprocessor = new dl::detect::yolo11PostProcessor(
            m_model, m_image_preprocessor, score_thr, nms_thr, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
    }
};

FireModel *s_model = nullptr;
}  // namespace

namespace detector {

bool load()
{
    float thr = FIRE_THRESHOLD < SMOKE_THRESHOLD ? FIRE_THRESHOLD : SMOKE_THRESHOLD;
    s_model = new FireModel(thr, NMS_THRESHOLD);
    ESP_LOGI(TAG, "model loaded, free PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return s_model != nullptr;
}

void run(const uint8_t *img, int width, int height, Verdict &out)
{
    out = Verdict();
    dl::image::img_t in = {};
    in.data = (void *)img;
    in.width = width;
    in.height = height;
    in.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;  // esp32-camera's RGB565 byte order

    std::list<dl::detect::result_t> &results = s_model->run(in);
    for (auto &r : results) {
        if (r.category < 0 || r.category >= NUM_CLASSES) {
            continue;
        }
        bool fire = r.category == CLASS_FIRE;
        if (r.score < (fire ? FIRE_THRESHOLD : SMOKE_THRESHOLD)) {
            continue;
        }
        r.limit_box(width, height);
        ESP_LOGI(TAG, "%s %.0f%% box [%d,%d,%d,%d]", CLASS_NAMES[r.category], r.score * 100, r.box[0], r.box[1],
                 r.box[2], r.box[3]);
        if (fire && r.score > out.best_fire) {
            out.best_fire = r.score;
        }
        if (r.category == CLASS_SMOKE && r.score > out.best_smoke) {
            out.best_smoke = r.score;
        }
        if (out.count < Verdict::MAX) {
            out.det[out.count++] = {r.category, r.score, r.box[0], r.box[1], r.box[2], r.box[3]};
        }
    }
}

}  // namespace detector
