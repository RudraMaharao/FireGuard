#pragma once
#include "brain/vision.h"
#include <vector>
namespace vision_service {
struct Evidence {
    VisionOut output;
    uint32_t at = 0;
};
struct Health { bool ready, busy; uint32_t imageAt, attemptedAt; int error; const char *reason; };
void start();
bool request_capture();
Health health();
Evidence evidence();
bool active();
bool save_sample();                     // saves the latest scored frame; never wakes a sleeping camera
bool photo(std::vector<uint8_t> &jpeg, uint32_t maxAgeMs = 4000); // short locked copy of cached JPEG
} // namespace vision_service
