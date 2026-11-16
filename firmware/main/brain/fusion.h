/* ============================================================================
   fusion.h — Model C: sensor fusion, the 0-100 Fire Risk Score, and the state
   machine. This is the decision intelligence; Models A and B only supply
   evidence to it.

   Four stages:
     1. temporal conditioning   (median -> asymmetric EWMA -> persistence)
     2. confidence weighting    (a dead channel weighs zero, no special case)
     3. aggregation             (noisy-OR base + corroboration - context damping)
     4. safety floors + state machine with hysteresis
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "physics_thresholds.h"

enum Chan { CH_FV = 0, CH_SG, CH_SV, CH_CG, CH_TE, CH_GG, N_CHAN };

enum SysState {
  ST_WARMING = 0, ST_SAFE, ST_NORMAL_COOKING, ST_WARNING, ST_SMOKE_WARNING,
  ST_GAS_SUSPECT, ST_GAS_DETECTED, ST_CO_HAZARD, ST_FIRE_SUSPECTED,
  ST_FIRE_DETECTED, ST_CRITICAL, ST_RECOVERING, ST_SENSOR_ERROR
};

static const char* STATE_NAME[] = {
  "WARMING", "SAFE", "NORMAL COOKING", "WARNING", "SMOKE WARNING",
  "GAS LEAK SUSPECT", "GAS LEAK DETECTED", "CO HAZARD", "FIRE SUSPECTED",
  "FIRE DETECTED", "CRITICAL", "RECOVERING", "SENSOR ERROR"
};

// Severity used only for LED/buzzer intensity and dwell rules.
static inline int severityOf(SysState s) {
  switch (s) {
    case ST_CRITICAL:                        return 5;
    case ST_FIRE_DETECTED: case ST_GAS_DETECTED: return 4;
    case ST_FIRE_SUSPECTED: case ST_CO_HAZARD:   return 3;
    case ST_SMOKE_WARNING: case ST_GAS_SUSPECT:  return 2;
    case ST_WARNING:                         return 1;
    default:                                 return 0;
  }
}

class Fusion {
public:
  // --- inputs (set these each tick) ---
  float raw[N_CHAN] = {0};        // evidence 0..1 from Models A and B
  float w[N_CHAN]   = {1,1,1,1,1,1};
  float kCook = 0, kSteam = 0;    // context, 0..1

  /* Context multipliers supplied by the caller from adapt.h and crossval.h.
     Both of those are constructed so that every one of these is EXACTLY 1.0
     once the channel's evidence is in the hazard band, so nothing that has been
     learned and nothing that has been seen can attenuate a real hazard. They
     are applied here, after confidence weighting, and the pre-context value is
     kept in eRaw[] so the difference is visible in the log rather than being
     something you have to infer. */
  float ctx[N_CHAN] = {1,1,1,1,1,1};
  float eRaw[N_CHAN] = {0};

  /* Shortens the entry dwell when the kitchen appears to be unattended. Never
     below MIN_DWELL_SCALE, and never applied to CRITICAL, which has no dwell. */
  float dwellScale = 1.0f;
  static constexpr float MIN_DWELL_SCALE = 0.55f;

  // --- outputs ---
  float cond[N_CHAN] = {0};       // conditioned evidence (post stage 1)
  float e[N_CHAN]    = {0};       // confidence-weighted evidence
  float sBase = 0, bonus = 0, damp = 1;
  float frs = 0, grs = 0, crs = 0;
  bool  rateEscalated = false;

  // Context may never pull the score below this fraction of what the raw
  // evidence alone would give.
  static constexpr float CTX_FLOOR = 0.70f;

  SysState state = ST_WARMING;
  bool     sensorError = false;
  uint32_t stateSince  = 0;

  void begin(uint32_t nowMs) { stateSince = nowMs; }

  // ======================================================= stage 1 =========
  void condition(float dt) {
    for (int i = 0; i < N_CHAN; i++) {
      // 1a. 5-sample median kills single-sample glitches outright
      med[i][medIdx[i]] = raw[i];
      medIdx[i] = (medIdx[i] + 1) % 5;
      float m[5];
      memcpy(m, med[i], sizeof m);
      for (int a = 0; a < 5; a++)
        for (int b = a + 1; b < 5; b++)
          if (m[b] < m[a]) { float t = m[a]; m[a] = m[b]; m[b] = t; }
      float x = m[2];

      // 1b. asymmetric EWMA: fast to alarm, slow to forgive
      float tau = (x > sm[i]) ? COND_ATTACK_S : COND_RELEASE_S;
      float a   = dt / tau; if (a > 1.0f) a = 1.0f;
      sm[i] += (x - sm[i]) * a;

      // 1c. persistence integrator: credit for STAYING elevated
      if (sm[i] > PERSIST_ON) pr[i] += dt / PERSIST_RISE_S;
      else                    pr[i] -= dt / PERSIST_FALL_S;
      pr[i] = constrain(pr[i], 0.0f, 1.0f);

      // A brand-new signal contributes only PERSIST_FLOOR of its magnitude.
      // This single line is why a one-second aerosol spike cannot raise an alarm.
      cond[i] = sm[i] * (PERSIST_FLOOR + (1.0f - PERSIST_FLOOR) * pr[i]);
    }
  }

  // ======================================================= stages 2-4 ======
  void aggregate(float dt) {
    for (int i = 0; i < N_CHAN; i++) {
      eRaw[i] = constrain(w[i] * cond[i], 0.0f, 1.0f);
      e[i]    = constrain(eRaw[i] * ctx[i], 0.0f, 1.0f);
    }

    // --- 3a. noisy-OR base -------------------------------------------------
    const float c[N_CHAN] = { C_FV, C_SG, C_SV, C_CG, C_TE, C_GG };
    float prod = 1.0f;
    for (int i = 0; i < N_CHAN; i++) prod *= (1.0f - c[i] * e[i]);
    sBase = 100.0f * (1.0f - prod);

    // --- 3b. corroboration: independent modalities agreeing is superadditive -
    bonus  = K_FV_GG * e[CH_FV] * e[CH_GG];
    bonus += K_FV_SG * e[CH_FV] * e[CH_SG];
    bonus += K_SV_SG * e[CH_SV] * e[CH_SG];
    bonus += K_SG_TE * e[CH_SG] * e[CH_TE];
    bonus += K_CG_SG * e[CH_CG] * e[CH_SG];
    bonus += K_FV_SV * e[CH_FV] * e[CH_SV];
    bonus += K_SV_TE * e[CH_SV] * e[CH_TE];
    bonus += K_GG_CG * e[CH_GG] * e[CH_CG];
    bonus += K_TRIPLE * e[CH_GG] * max(e[CH_SV], e[CH_SG]) * e[CH_FV];

    // --- 3c. context suppression, floored so it can never mute a real fire --
    damp = constrain(1.0f - D_COOK * kCook - D_STEAM * kSteam, D_FLOOR, 1.0f);

    float s = (sBase + bonus) * damp;

    // --- 3d. hard safety floors, applied AFTER damping ----------------------
    if (e[CH_FV] >= 0.80f)                                    s = max(s, 75.0f);
    if (e[CH_FV] >= 0.80f && (e[CH_SG] >= 0.50f || e[CH_SV] >= 0.60f))
                                                              s = max(s, 88.0f);
    if (e[CH_GG] >= 0.85f && e[CH_FV] >= 0.50f)               s = max(s, 90.0f);
    if (e[CH_CG] >= 0.85f)                                    s = max(s, 55.0f);

    // --- 3d-bis. the physics floor -----------------------------------------
    // Context damping, adaptation and person presence all act on e[]. Each is
    // individually fenced, but they multiply, and three fenced factors can
    // still compound near the fence. So the score is also floored against what
    // the UNMODIFIED evidence would have produced through the same noisy-OR.
    // Whatever the system has learned about your kitchen, it cannot score a
    // situation lower than the raw evidence justifies by more than CTX_FLOOR.
    {
      float prodRaw = 1.0f;
      for (int i = 0; i < N_CHAN; i++) prodRaw *= (1.0f - c[i] * eRaw[i]);
      float sRaw = 100.0f * (1.0f - prodRaw);
      s = max(s, CTX_FLOOR * sRaw);
    }

    // --- 3e. rate-of-rise escalation ---------------------------------------
    // A fast-developing event is more dangerous than a static one at the same
    // score. Slope measured over the last 3 seconds.
    slope[slopeIdx] = s; slopeIdx = (slopeIdx + 1) % SLOPE_N;
    if (slopeCount < SLOPE_N) slopeCount++;
    rateEscalated = false;
    if (slopeCount >= SLOPE_N) {
      float oldest = slope[slopeIdx];
      float rate = (s - oldest) / (SLOPE_N * dt);
      if (rate > 8.0f) { s += 10.0f; rateEscalated = true; }
    }

    frs = constrain(s, 0.0f, 100.0f);
  }

  // Gas and CO are reported on their own axes. A gas leak is its own emergency,
  // not a weak fire signal.
  void auxScores(bool lpgSignature) {
    grs = 100.0f * e[CH_GG] * (1.0f + 0.10f * e[CH_CG])
        + (lpgSignature ? 12.0f * e[CH_GG] : 0.0f);
    crs = 100.0f * e[CH_CG] * (1.0f + 0.10f * e[CH_TE]);
    grs = constrain(grs, 0.0f, 100.0f);
    crs = constrain(crs, 0.0f, 100.0f);
  }

  // ======================================================= state machine ===
  void updateState(uint32_t now, bool warming, bool anyInvalid) {
    if (warming) { setState(ST_WARMING, now); return; }
    sensorError = anyInvalid;

    bool flame = (e[CH_FV] >= FLAME_CONFIRM);

    // Entry dwell: a candidate must hold before the top states latch.
    trackCandidate(candFire, now,
                   (frs >= TH_FIRE_DETECTED && flame) || frs >= TH_FIRE_NO_VISION);
    trackCandidate(candGas, now, grs >= TH_GAS_DETECTED);

    float ds = constrain(dwellScale, MIN_DWELL_SCALE, 1.0f);
    uint32_t entryFire = (uint32_t)(ENTRY_FIRE_MS * ds);
    uint32_t entryGas  = (uint32_t)(ENTRY_GAS_MS  * ds);

    SysState want;
    if (((frs >= TH_CRITICAL && flame && grs >= TH_GAS_DETECTED) ||
          frs >= TH_CRITICAL_ALONE))                             want = ST_CRITICAL;
    else if (candFire.held(now, entryFire))                      want = ST_FIRE_DETECTED;
    else if (candGas.held(now, entryGas))                        want = ST_GAS_DETECTED;
    else if (crs >= TH_CO_HAZARD)                                want = ST_CO_HAZARD;
    else if (frs >= TH_FIRE_SUSPECTED)                           want = ST_FIRE_SUSPECTED;
    else if (frs >= TH_SMOKE_WARNING)                            want = ST_SMOKE_WARNING;
    else if (grs >= TH_GAS_SUSPECT)                              want = ST_GAS_SUSPECT;
    else if (frs >= TH_WARNING)                                  want = ST_WARNING;
    else if (frs >= TH_NORMAL_COOKING)                           want = ST_NORMAL_COOKING;
    else                                                         want = ST_SAFE;

    // A dead channel makes the score meaningless, so say so — but only when
    // nothing is actually happening. A real hazard always outranks the notice.
    if (anyInvalid && frs < TH_WARNING && grs < TH_GAS_SUSPECT) want = ST_SENSOR_ERROR;

    int curSev = severityOf(state), wantSev = severityOf(want);

    // ----- escalation is NEVER delayed, gated or held ----------------------
    if (wantSev > curSev) { setState(want, now); return; }

    // ----- hysteresis and dwell apply on the way DOWN only -----------------
    if (wantSev < curSev) {
      if (!dwellSatisfied(now)) return;                 // still in minimum dwell
      if (!hysteresisCleared()) return;                 // not far enough below
      if (curSev >= 2) { setState(ST_RECOVERING, now); return; }   // leaving an alarm
    }
    if (state == ST_RECOVERING && now - stateSince < RECOVER_MS) return;
    if (state != want) setState(want, now);
  }

  const char* stateName() const { return STATE_NAME[state]; }

private:
  float med[N_CHAN][5] = {{0}};
  int   medIdx[N_CHAN] = {0};
  float sm[N_CHAN] = {0};
  float pr[N_CHAN] = {0};

  static const int SLOPE_N = 6;                 // 3 s at 2 Hz
  float slope[SLOPE_N] = {0};
  int   slopeIdx = 0, slopeCount = 0;

  struct Candidate {
    bool active = false; uint32_t since = 0;
    bool held(uint32_t now, uint32_t ms) const { return active && (now - since) >= ms; }
  };
  Candidate candFire, candGas;

  void trackCandidate(Candidate &c, uint32_t now, bool cond_) {
    if (cond_) { if (!c.active) { c.active = true; c.since = now; } }
    else c.active = false;
  }

  void setState(SysState s, uint32_t now) { state = s; stateSince = now; }

  bool dwellSatisfied(uint32_t now) const {
    int sev = severityOf(state);
    if (sev >= 4) return (now - stateSince) >= DWELL_ALARM_MS;
    if (sev >= 1) return (now - stateSince) >= DWELL_MIN_MS;
    return true;
  }

  // Must fall HYST_DROP points below the threshold that got us here.
  bool hysteresisCleared() const {
    switch (state) {
      case ST_CRITICAL:       return frs < TH_CRITICAL       - HYST_DROP;
      case ST_FIRE_DETECTED:  return frs < TH_FIRE_DETECTED  - HYST_DROP;
      case ST_GAS_DETECTED:   return grs < TH_GAS_DETECTED   - HYST_DROP;
      case ST_CO_HAZARD:      return crs < TH_CO_HAZARD      - HYST_DROP;
      case ST_FIRE_SUSPECTED: return frs < TH_FIRE_SUSPECTED - HYST_DROP;
      case ST_SMOKE_WARNING:  return frs < TH_SMOKE_WARNING  - HYST_DROP;
      case ST_GAS_SUSPECT:    return grs < TH_GAS_SUSPECT    - HYST_DROP;
      case ST_WARNING:        return frs < TH_WARNING        - HYST_DROP;
      default:                return true;
    }
  }
};
