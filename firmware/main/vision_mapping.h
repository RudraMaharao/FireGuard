#pragma once
#include "brain/vision.h"
#include "detection_types.h"
// Per-box mapping: a small burner cannot hide a second, larger fire box.
inline VisionOut map_yolo(const VisionOut &classical, const Verdict &v, int w, int h) {
    VisionOut o = classical;
    o.openFlame = o.burnerFlame = o.smokeLight = o.smokeHeavy = 0;
    for (int i = 0; i < v.count; ++i) {
        const auto &d = v.det[i];
        float area = float(max(0, d.x2 - d.x1) * max(0, d.y2 - d.y1)) / (w * h);
        float cy = float(d.y1 + d.y2) / (2 * h);
        if (d.cls == 0) {
            if (area <= 0.046833f && cy >= 0.55f)
                o.burnerFlame = max(o.burnerFlame, d.score);
            else
                o.openFlame = max(o.openFlame, d.score);
        }
        if (d.cls == 1) {
            o.smokeLight = max(o.smokeLight, d.score);
            if (area >= 0.15f)
                o.smokeHeavy = max(o.smokeHeavy, d.score);
        }
    }
    return o;
}
