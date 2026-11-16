/* ============================================================================
   sensor_features.h — the 24-feature vector Model A consumes.

   CRITICAL: this file is the single source of truth for feature definitions.
   training/feature_ref.py mirrors it line for line. If you change one, change
   both, or you get train/serve skew — the most common and most invisible
   failure mode in embedded ML.
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <math.h>

#define RING_N     512            // 256 s of history at 2 Hz
#define N_FEAT      24

struct Sample { float dev[3]; float t; float ah; };

// Absolute humidity in g/m^3. This, not relative humidity, is the physically
// meaningful quantity — and it is what separates a steam plume from smoke.
static inline float absHumidity(float tC, float rh) {
  if (isnan(tC) || isnan(rh)) return NAN;
  return 6.112f * expf(17.67f * tC / (tC + 243.5f)) * rh * 2.1674f / (273.15f + tC);
}

class FeatureRing {
public:
  Sample buf[RING_N];
  int    head = 0;
  int    count = 0;

  void push(const float dev[3], float t, float ah) {
    Sample &s = buf[head];
    s.dev[0] = dev[0]; s.dev[1] = dev[1]; s.dev[2] = dev[2];
    s.t = t; s.ah = ah;
    head = (head + 1) % RING_N;
    if (count < RING_N) count++;
  }

  // n samples ago (0 = most recent). Clamped to the oldest available sample.
  const Sample& back(int n) const {
    if (n >= count) n = count - 1;
    if (n < 0) n = 0;
    int i = (head - 1 - n + 2 * RING_N) % RING_N;
    return buf[i];
  }

  bool ready() const { return count >= 8; }

  /* ---------------------------------------------------------------------
     out[0..2]   normalised level        dev_i
     out[3..5]   short rate              dev_i(now) - dev_i(-10 s)
     out[6..8]   long rate               dev_i(now) - dev_i(-120 s)
     out[9..11]  dispersion              stddev(dev_i) over 60 s
     out[12..14] persistence             fraction of last 90 s above 0.5*scale
     out[15..17] cross-channel           dev2-dev135, dev7-dev2, dev7-dev135
     out[18]     temperature             degC
     out[19]     absolute humidity       g/m^3
     out[20]     dT / 60 s
     out[21]     dAH / 60 s
     out[22]     dP / 300 s              (passed in, EWMA-derived)
     out[23]     minutes since warm-up   capped at 60
     --------------------------------------------------------------------- */
  void compute(float *out, const float devScale[3], float dP300, float warmMin) const {
    const int S10 = 20, S60 = 120, S90 = 180, S120 = 240;   // samples at 2 Hz
    const Sample &now = back(0);

    for (int c = 0; c < 3; c++) {
      out[0 + c] = now.dev[c];
      out[3 + c] = now.dev[c] - back(S10).dev[c];
      out[6 + c] = now.dev[c] - back(S120).dev[c];

      int n60 = min(S60, count);
      double m = 0;
      for (int i = 0; i < n60; i++) m += back(i).dev[c];
      m /= max(1, n60);
      double v = 0;
      for (int i = 0; i < n60; i++) { double d = back(i).dev[c] - m; v += d * d; }
      out[9 + c] = sqrtf((float)(v / max(1, n60)));

      int n90 = min(S90, count), hits = 0;
      float thr = 0.5f * max(devScale[c], 0.01f);
      for (int i = 0; i < n90; i++) if (back(i).dev[c] > thr) hits++;
      out[12 + c] = (float)hits / max(1, n90);
    }

    // The selectivity substitute. No single MQ channel identifies a gas, but the
    // DIFFERENCES between them carry a crude signature: LPG lifts MQ-2 hard and
    // MQ-7 little; combustion CO does close to the reverse.
    out[15] = now.dev[0] - now.dev[2];
    out[16] = now.dev[1] - now.dev[0];
    out[17] = now.dev[1] - now.dev[2];

    out[18] = now.t;
    out[19] = now.ah;
    out[20] = now.t  - back(S60).t;
    out[21] = now.ah - back(S60).ah;
    out[22] = dP300;
    out[23] = min(warmMin, 60.0f);

    for (int i = 0; i < N_FEAT; i++) if (isnan(out[i]) || isinf(out[i])) out[i] = 0.0f;
  }
};
