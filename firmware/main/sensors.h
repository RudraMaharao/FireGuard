#pragma once
#include "brain/fusion.h"
#include "brain/vision.h"
#include "driver/i2c_master.h"
#include <stdint.h>
namespace sensors {
struct Snapshot {
    uint32_t ms = 0;
    float dev[3] = {}, ratio[3] = {}, devScale[3] = {};
    float tC = NAN, rh = NAN, hPa = NAN;
    float aGas = 0, aCo = NAN, aSmoke = 0, mGas = 0, mCo = NAN, mSmoke = 0, coDose = NAN, dT60 = 0, dAH60 = 0,
          dRH60 = 0;
    float visionCredit = 0, cond[6] = {}, w[6] = {}, sBase = 0, bonus = 0, damp = 1, frs = 0, grs = 0,
          crs = NAN;
    uint8_t hint = 0, state = ST_WARMING, flags = 2, label = 0;
    bool adcOK=false, mq2OK=false, mq135OK=false;
    float adcMv2=NAN, adcMv135=NAN;
    bool cameraRequested = false, cameraValid = false, muted = false, calibrated = false;
    uint32_t burninSec = 0, visionAgeMs = 0;
    VisionOut vision;
};
enum Command {
    MUTE,
    CYCLE_LABEL,
    CAPTURE_R0,
    PRINT_STATUS,
    PRINT_PHYSICS,
    PRINT_ADAPT,
    RESET_ADAPT,
    PRINT_CROSSVAL,
    PRINT_BURNIN
};
void start(i2c_master_bus_handle_t bus);
Snapshot snapshot();
bool command(Command c);
} // namespace sensors
