/* ============================================================================
   model_a.h — gas, CO and combustion intelligence.

   Ships with thresholds DERIVED FROM PHYSICS, not guessed and not fitted. The
   whole derivation lives in training/physics/ and its output is the generated
   physics_thresholds.h that this file consumes. Nothing here was tuned against
   recorded data, which is what makes it safe to ship before any data exists.

   Three ideas carry the file.

   1. THE SCALE-FREE DEVIATION.
      A MOX element obeys Rs/R0 = A * C^-B. A carries the heater voltage, the
      load resistor, the element's age and its absolute resistance — everything
      unknowable on a 3.3 V build. B is the log-log slope, a property of the
      surface chemistry. The firmware works in dev = ln(baseline / (Rs/R0)),
      and in that quantity A cancels exactly:

            dev = B * ln(C / C_base)

      So "10 % of the lower explosive limit" converts into a z-score of dev
      without ever knowing what R0 is. That is the entire reason the numbers
      below can be defended.

   2. LINEAR CONTRASTS INSTEAD OF THRESHOLDS PER CHANNEL.
      No MQ sensor is selective; every one of them answers to every reducing
      gas. But a fuel leak has a fixed shape across the three channels —
      z(MQ-7) ~ 2.4 z(MQ-2) and z(MQ-135) ~ 1.5 z(MQ-2) — so subtracting that
      shape leaves nothing for fuel and a large positive residue for anything
      else. Three contrasts, one per hazard, each of which is near zero for the
      other two. This replaces the older difference-and-gate scheme, which
      failed at high concentration: the channels saturate at different rates, so
      the ratios drift together as the event grows and a gate placed between two
      classes weakens exactly when it matters most.

   3. CARBON MONOXIDE IS A DOSE, NOT A LEVEL.
      EN 50291's response envelope looks like four unrelated numbers until you
      push them through Stewart's equation, at which point every one of them
      lands near 8-10 % carboxyhaemoglobin. 50 ppm for 90 minutes is as harmful
      as 300 ppm for 3. A level threshold cannot express that, so there is a
      second, integrating channel with the elimination half-life of COHb built
      into its decay.

   When you have collected data and trained the real model, set
   USE_TRAINED_MODEL_A to 1. The interface is identical and nothing downstream
   changes. The physics path remains the floor: see fusion.h.
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <math.h>
#include "sensor_features.h"
#include "physics_thresholds.h"

#define USE_TRAINED_MODEL_A 0      // <-- set to 1 after training

#if USE_TRAINED_MODEL_A
  #include "model_a_trained.h"     // generated; defines ma_gas / ma_co / ma_smoke
#endif

struct SensorScores {
  float gas = 0, co = 0, smoke = 0;
  // Diagnostics, logged and printed. These are what make a score explainable
  // at 2 a.m. instead of merely numeric.
  float mGas = 0, mCo = 0, mSmoke = 0;   // the contrasts
  float coDose = 0;                       // z-minutes
  float coLevelPart = 0, coDosePart = 0;
  bool  steamVetoed = false;
};

static inline float pzSig(float x, float k) {
  float t = x / (k > 1e-6f ? k : 1e-6f);
  if (t >  40.0f) return 1.0f;
  if (t < -40.0f) return 0.0f;
  return 1.0f / (1.0f + expf(-t));
}

class ModelA {
public:
  void begin() { dose = 0.0f; }

  /* ---------------------------------------------------------------------
     f          the 24-feature vector from sensor_features.h
     devScale   per-channel quiet-air noise width, the z-score denominator
     dt         seconds since the last call
     --------------------------------------------------------------------- */
  SensorScores run(const float *f, const float devScale[3], float dt) {
    SensorScores s;

    const float z2   = f[0] / fmaxf(devScale[0], 1e-3f);
    const float z7   = f[1] / fmaxf(devScale[1], 1e-3f);
    const float z135 = f[2] / fmaxf(devScale[2], 1e-3f);
    const float dT60  = f[20];
    const float dAH60 = f[21];

#if USE_TRAINED_MODEL_A
    float out[1];
    ma_gas(f, out);   s.gas   = out[0];
    ma_co(f, out);    s.co    = out[0];
    ma_smoke(f, out); s.smoke = out[0];
    // The dose channel is physics, not learning, and stays live regardless:
    // no amount of training data from one kitchen contains a CO poisoning.
    s.mCo = z7 - PZ_R1_FUEL * z2;
    integrateDose(s.mCo, dt);
    s.coDose = dose;
    s.co = noisyOr(s.co, pzSig(dose - PZ_CO_DOSE_ALARM, PZ_CO_DOSE_ALARM * 0.25f));
#else
    // ---- the three contrasts --------------------------------------------
    // Residues clipped at zero: a channel reading LOW relative to the fuel
    // signature must not be allowed to inflate the fuel score.
    const float resSmoke = fmaxf(z135 - PZ_R2_FUEL * z2, 0.0f);
    const float resCo    = fmaxf(z7   - PZ_R1_FUEL * z2, 0.0f);

    s.mGas   = z2 - PZ_GAS_PENALTY * (resSmoke + resCo);
    s.mCo    = z7   - PZ_R1_FUEL * z2;
    s.mSmoke = z135 - PZ_R2_FUEL * z2;

    // ---- gas -------------------------------------------------------------
    s.gas = pzSig(s.mGas - PZ_X0_GAS, PZ_K_GAS);

    // ---- CO: level OR dose ------------------------------------------------
    integrateDose(s.mCo, dt);
    s.coDose      = dose;
    s.coLevelPart = pzSig(s.mCo - PZ_X0_CO, PZ_K_CO);
    s.coDosePart  = pzSig(dose - PZ_CO_DOSE_ALARM, PZ_CO_DOSE_ALARM * 0.25f);
    s.co = noisyOr(s.coLevelPart, s.coDosePart);

    // ---- smoke ------------------------------------------------------------
    s.smoke = pzSig(s.mSmoke - PZ_X0_SMOKE, PZ_K_SMOKE);

    // Heat corroborates combustion. Scaled against what a fire does, so a
    // burner's contribution is a rounding error and a pan fire's is not.
    s.smoke *= (1.0f + PZ_THERMAL_BOOST *
                constrain(dT60 / fmaxf(PZ_DT60_FIRE, 1e-3f), 0.0f, 1.0f));

    // ---- the steam veto ----------------------------------------------------
    // A surge in ABSOLUTE humidity with MQ-2 barely moving is a pot, not a
    // fire. Relative humidity would be the wrong quantity here: a fire raises
    // temperature, which drives relative humidity DOWN while absolute humidity
    // stays flat, so the two hazards separate in absolute terms and overlap in
    // relative ones.
    if (dAH60 > PZ_DAH60_STEAM && z2 < PZ_STEAM_Z2_MAX) {
      s.smoke *= 0.45f;
      s.gas   *= 0.60f;
      s.steamVetoed = true;
    }
#endif

    s.gas   = constrain(s.gas,   0.0f, 1.0f);
    s.co    = constrain(s.co,    0.0f, 1.0f);
    s.smoke = constrain(s.smoke, 0.0f, 1.0f);
    return s;
  }

  // Cleared when the air has been demonstrably clean for a while, so the dose
  // does not creep up over months of ordinary cooking.
  void decayDoseIdle(float dt) { dose *= expf(-dt / PZ_CO_DOSE_TAU_S); }

  float doseNow() const { return dose; }

  void print(Stream &out, const SensorScores &s) const {
    out.println(F("--- model A (physics) -----------------------------------"));
    out.printf("  contrasts   gas %7.1f   co %7.1f   smoke %7.1f\n",
               s.mGas, s.mCo, s.mSmoke);
    out.printf("  knees       gas %7.1f   co %7.1f   smoke %7.1f\n",
               PZ_X0_GAS, PZ_X0_CO, PZ_X0_SMOKE);
    out.printf("  scores      gas %7.3f   co %7.3f   smoke %7.3f%s\n",
               s.gas, s.co, s.smoke, s.steamVetoed ? "   [steam veto]" : "");
    out.printf("  CO dose     %.0f of %.0f z-min   (level %.2f, dose %.2f)\n",
               s.coDose, PZ_CO_DOSE_ALARM, s.coLevelPart, s.coDosePart);
    out.println(F("  what the knees mean, from the derivation:"));
    out.printf("    LPG 10%% LEL -> %.2f    LPG 25%% LEL -> %.2f\n",
               PZ_AUDIT_GAS_10LEL, PZ_AUDIT_GAS_25LEL);
    out.printf("    CO 30 ppm   -> %.2f    CO 100 ppm  -> %.2f    CO 300 ppm -> %.2f\n",
               PZ_AUDIT_CO_30, PZ_AUDIT_CO_100, PZ_AUDIT_CO_300);
  }

private:
  float dose = 0.0f;

  void integrateDose(float mCo, float dt) {
    float x = mCo > 0.0f ? mCo : 0.0f;
    dose = dose * expf(-dt / PZ_CO_DOSE_TAU_S) + x * dt / 60.0f;
    if (dose > 20.0f * PZ_CO_DOSE_ALARM) dose = 20.0f * PZ_CO_DOSE_ALARM;
  }

  static float noisyOr(float a, float b) { return 1.0f - (1.0f - a) * (1.0f - b); }
};
