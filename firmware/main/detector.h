// Runs the fire/smoke YOLO11 model (fire_det.espdl, embedded in flash) with ESP-DL.
#pragma once
#include "detection_types.h"
#include <stdint.h>

namespace detector {
bool load();  // call once at boot (the model then stays in memory while the chip sleeps)
// img: big-endian RGB565 photo. Fills `out` with detections above the thresholds.
void run(const uint8_t *img, int width, int height, Verdict &out);
}  // namespace detector
