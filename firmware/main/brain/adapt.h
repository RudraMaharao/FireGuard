/* ============================================================================
   adapt.h — the part that learns your kitchen, and the fence around it.

   Every kitchen is different. Mustard oil at 250 C, a tadka of asafoetida, a
   pressure cooker venting twice a day — all of these produce real signal on
   real sensors, and a threshold derived from physics has no way to know that
   yours does it every evening at seven. So the system has to adapt.

   The danger is obvious once stated: a system that can move its own thresholds
   can learn its way into silence. Every false alarm it suppresses looks, from
   the inside, exactly like success. Left unconstrained, an adaptive alarm
   converges on never alarming, because that is the state with no complaints.

   The fence, therefore, is the design:

     1. ADAPTATION IS ONE-DIRECTIONAL. It may attenuate evidence in the nuisance
        band. It may never amplify sensitivity, and it may never move a physics
        threshold. PZ_* constants are read-only to this file.

     2. IT CANNOT REACH THE HAZARD BAND. Above ADAPT_HARD_KEEP — the evidence
        level that the physics says corresponds to a real, published hazard —
        the attenuation is exactly 1.0. Not "nearly", not "usually". The
        multiplier is computed as a function that is identically one there, so
        there is no parameter value and no accumulated learning that can pull it
        down. A kitchen that has taught the system to ignore everything still
        alarms on a pan fire.

     3. IT IS BOUNDED. alpha lives in [0, ADAPT_ALPHA_MAX]. Even fully saturated,
        the nuisance band is attenuated by 45 %, not silenced.

     4. IT DECAYS. Learning leaks back toward zero with a multi-week time
        constant. A kitchen that stops producing nuisance signal — you replaced
        the extractor, you moved the sensor — recovers its sensitivity on its
        own rather than staying desensitised forever by evidence that no longer
        applies.

     5. IT IS AUDITABLE. Press 'a' on the serial console and it prints what it
        has learned, why, and what the physics floor is. A safety system whose
        current thresholds cannot be read out is not a safety system.
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "physics_thresholds.h"

// Evidence at or above this is hazard-band and is never attenuated. It is the
// conditioned-evidence level that the derivation's own anchors produce for a
// published limit: 10 % LEL, 100 ppm CO, 4.5 %/m obscuration.
#define ADAPT_HARD_KEEP     0.55f

#define ADAPT_ALPHA_MAX     0.45f    // deepest attenuation in the nuisance band
#define ADAPT_Q_TARGET      0.98f    // quantile of benign evidence we track
#define ADAPT_Q_REF         0.22f    // benign q98 the physics model predicts
#define ADAPT_Q_ETA         0.0009f  // Robbins-Monro step for the quantile
#define ADAPT_GAIN          1.30f    // alpha per unit of excess benign quantile
#define ADAPT_DECAY_S       1209600.0f  // 14 days: learning leaks back to zero
#define ADAPT_MUTE_STEP     0.045f   // one mute-after-alarm is worth this much
#define ADAPT_MIN_SAMPLES   7200UL   // 1 h of benign observation before acting
#define ADAPT_SAVE_MS       1800000UL// persist every 30 min, not every tick

enum AdaptChan { AD_GAS = 0, AD_CO, AD_SMOKE, AD_VIS, AD_N };

static const char* ADAPT_NAME[AD_N] = { "gas", "co", "smoke", "vision" };

class Adapt {
public:
  float q[AD_N]    = { ADAPT_Q_REF, ADAPT_Q_REF, ADAPT_Q_REF, ADAPT_Q_REF };
  float alpha[AD_N] = {0, 0, 0, 0};
  uint32_t nBenign = 0;
  uint32_t muteEvents = 0;
  bool     enabled = true;

  void begin() {
    prefs.begin("kfg-adapt", false);
    if (prefs.getUChar("ver", 0) == SCHEMA) {
      for (int i = 0; i < AD_N; i++) {
        q[i]     = prefs.getFloat(keyQ(i), ADAPT_Q_REF);
        alpha[i] = prefs.getFloat(keyA(i), 0.0f);
      }
      nBenign    = prefs.getULong("n", 0);
      muteEvents = prefs.getULong("mutes", 0);
    }
    clampAll();
    lastSave = millis();
  }

  /* ---------------------------------------------------------------------
     The multiplier applied to one channel's evidence.

     Identically 1.0 at and above ADAPT_HARD_KEEP. Below it, the attenuation
     ramps in smoothly so there is no discontinuity for the state machine's
     hysteresis to chatter across.
     --------------------------------------------------------------------- */
  float scale(AdaptChan c, float evidence) const {
    if (!enabled || evidence >= ADAPT_HARD_KEEP) return 1.0f;
    float below = 1.0f - evidence / ADAPT_HARD_KEEP;   // 0 at the fence, 1 at zero
    return 1.0f - alpha[c] * below;
  }

  /* ---------------------------------------------------------------------
     Called every tick. `benign` must mean CONFIRMED benign, not merely
     "not currently alarming" — see sensor_node.ino for how that is decided.
     Learning from unlabelled quiet time would let a slow leak teach the
     system that slow leaks are normal.
     --------------------------------------------------------------------- */
  void observe(bool benign, const float ev[AD_N], float dt) {
    decay(dt);
    if (!benign || !enabled) return;
    nBenign++;
    for (int i = 0; i < AD_N; i++) {
      // Robbins-Monro online quantile: q rises when a sample exceeds it and
      // falls otherwise, converging on the ADAPT_Q_TARGET quantile with O(1)
      // memory. No histogram, no window, nothing to size.
      q[i] += ADAPT_Q_ETA * ((ev[i] > q[i] ? 1.0f : 0.0f) - (1.0f - ADAPT_Q_TARGET));
      q[i] = constrain(q[i], 0.0f, ADAPT_HARD_KEEP);
    }
    if (nBenign >= ADAPT_MIN_SAMPLES) recompute();
  }

  /* A mute press shortly after an escalation is the strongest false-alarm
     label available without asking the user anything. It is worth far more than
     the same amount of quiet time, so it gets its own fast path. */
  void noteMuteAfterAlarm(AdaptChan blame) {
    if (!enabled) return;
    muteEvents++;
    alpha[blame] = constrain(alpha[blame] + ADAPT_MUTE_STEP, 0.0f, ADAPT_ALPHA_MAX);
  }

  void reset() {
    for (int i = 0; i < AD_N; i++) { q[i] = ADAPT_Q_REF; alpha[i] = 0.0f; }
    nBenign = 0; muteEvents = 0;
    persist();
  }

  void tickSave() {
    if (millis() - lastSave > ADAPT_SAVE_MS) { persist(); lastSave = millis(); }
  }

  void print(Stream &s) const {
    s.println(F("--- adaptation ------------------------------------------"));
    s.printf("  enabled %s   benign samples %lu (%.1f h)   mutes %lu\n",
             enabled ? "yes" : "no", (unsigned long)nBenign,
             nBenign / 7200.0f, (unsigned long)muteEvents);
    s.printf("  %-8s %8s %8s %8s\n", "channel", "q98", "alpha", "at e=0.30");
    for (int i = 0; i < AD_N; i++)
      s.printf("  %-8s %8.3f %8.3f %8.3f\n", ADAPT_NAME[i], q[i], alpha[i],
               scale((AdaptChan)i, 0.30f));
    s.printf("  hard keep at e >= %.2f: attenuation is exactly 1.000 there,\n",
             ADAPT_HARD_KEEP);
    s.println(F("  for every channel, at every value these numbers can reach."));
    s.printf("  physics floors (read-only): gas x0 %.1f  co x0 %.1f  smoke x0 %.1f\n",
             PZ_FLOOR_X0_GAS, PZ_FLOOR_X0_CO, PZ_FLOOR_X0_SMOKE);
  }

private:
  static const uint8_t SCHEMA = 2;
  Preferences prefs;
  uint32_t lastSave = 0;

  static const char* keyQ(int i) {
    static char b[6]; snprintf(b, sizeof b, "q%d", i); return b;
  }
  static const char* keyA(int i) {
    static char b[6]; snprintf(b, sizeof b, "a%d", i); return b;
  }

  void recompute() {
    for (int i = 0; i < AD_N; i++) {
      float excess = q[i] - ADAPT_Q_REF;
      float want = (excess > 0) ? ADAPT_GAIN * excess : 0.0f;
      // Only ever move UP toward `want`; the mute fast path may have put alpha
      // above it and that evidence is not discarded by a quiet afternoon.
      if (want > alpha[i]) alpha[i] += (want - alpha[i]) * 0.02f;
      alpha[i] = constrain(alpha[i], 0.0f, ADAPT_ALPHA_MAX);
    }
  }

  void decay(float dt) {
    float k = expf(-dt / ADAPT_DECAY_S);
    for (int i = 0; i < AD_N; i++) {
      alpha[i] *= k;
      q[i] = ADAPT_Q_REF + (q[i] - ADAPT_Q_REF) * k;
    }
  }

  void clampAll() {
    for (int i = 0; i < AD_N; i++) {
      if (isnan(alpha[i]) || isinf(alpha[i])) alpha[i] = 0.0f;
      if (isnan(q[i]) || isinf(q[i])) q[i] = ADAPT_Q_REF;
      alpha[i] = constrain(alpha[i], 0.0f, ADAPT_ALPHA_MAX);
      q[i]     = constrain(q[i], 0.0f, ADAPT_HARD_KEEP);
    }
  }

  void persist() {
    prefs.putUChar("ver", SCHEMA);
    for (int i = 0; i < AD_N; i++) {
      prefs.putFloat(keyQ(i), q[i]);
      prefs.putFloat(keyA(i), alpha[i]);
    }
    prefs.putULong("n", nBenign);
    prefs.putULong("mutes", muteEvents);
  }
};
