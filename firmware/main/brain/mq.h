/* ============================================================================
   mq.h — MQ analogue front end: ADC -> Rs -> Rs/R0 -> baseline-normalised
   deviation. Everything downstream consumes `dev`, never raw counts.
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <math.h>
#include "config.h"

struct MQChannel {
  int   pin;
  float r0        = 0;      // clean-air resistance, from NVS
  float rs        = 0;
  float ratio     = 1;      // Rs/R0
  float baseline  = 1;      // slow EWMA of ratio, FROZEN during elevation
  float dev       = 0;      // log(baseline/ratio): >0 means more reducing gas
  float devScale  = 0.05f;  // slow EWMA of |dev| in quiet air -> the z denominator
  float z         = 0;      // dev / devScale
  bool  valid     = false;
  bool  baseInit  = false;

  void begin(int p, float r0_) {
    pin = p; r0 = r0_;
    analogSetPinAttenuation(pin, ADC_11db);
  }

  // 64-sample average in calibrated millivolts.
  float readMv() const {
    uint32_t acc = 0;
    for (int i = 0; i < 64; i++) { acc += analogReadMilliVolts(pin); }
    return acc / 64.0f;
  }

  // Returns false if the channel looks disconnected or shorted.
  bool sample() {
    float mv = readMv();
    if (mv < 40.0f || mv > MQ_VCC_MV - 40.0f) { valid = false; return false; }
    rs    = MQ_RL_OHM * (MQ_VCC_MV - mv) / mv;
    ratio = (r0 > 1.0f) ? (rs / r0) : rs;      // uncalibrated fallback: raw Rs
    if (ratio < 1e-4f) ratio = 1e-4f;
    valid = true;
    if (!baseInit) { baseline = ratio; baseInit = true; }
    dev = logf(baseline / ratio);
    z   = dev / max(devScale, 0.01f);
    return true;
  }

  /* The baseline must NOT follow a slow leak, or the deviation goes to zero and
     the system is blind to exactly the most dangerous event class. It updates
     only while the caller says the system is quiet. Same rule for devScale. */
  void updateBaseline(float dt, bool allowed) {
    if (!valid || !allowed) return;
    float a = dt / BASE_TAU_S;
    if (a > 1.0f) a = 1.0f;
    baseline += (ratio - baseline) * a;
    devScale += (fabsf(dev) - devScale) * a;
    if (devScale < 0.01f) devScale = 0.01f;
  }
};
