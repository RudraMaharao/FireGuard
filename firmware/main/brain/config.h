#pragma once
#define MQ_RL_OHM 1000.0f
#define ADS_MQ2_CHANNEL 0
#define ADS_MQ135_CHANNEL 2
// No MQ is wired to an ESP32 ADC pin.
// ------------------------------------------------------------- hardware ----
#define MQ_VCC_MV   5000.0f          // MQ modules on 5 V; AO divided by two before ADS1115
#define TICK_MS     500              // control loop period -> 2 Hz
#define WARMUP_S    300              // 5 min preheat before scoring starts

// ------------------------------------------------------ baseline tracking --
#define BASE_TAU_S      1800.0f      // 30 min EWMA
#define BASE_FREEZE_Z   1.0f         // freeze baseline above this deviation z
#define BASE_FREEZE_MAX 21600UL      // 6 h cap, then SENSOR_ERROR

// ------------------------------------------------- temporal conditioning ---
#define COND_ATTACK_S    2.0f        // fast to alarm
#define COND_RELEASE_S  20.0f        // slow to forgive
#define PERSIST_ON       0.12f       // evidence level that counts as "elevated"
#define PERSIST_RISE_S  12.0f        // seconds of persistence for full credit
#define PERSIST_FALL_S  45.0f
#define PERSIST_FLOOR    0.40f       // a brand-new signal contributes only 40%

// ------------------------------------------------- fusion severity ceilings-
// Maximum FRS each channel can produce ALONE, at full evidence and confidence.
#define C_FV  0.85f                  // visual open flame
#define C_SG  0.45f                  // sensor smoke / combustion
#define C_SV  0.40f                  // visual smoke
#define C_CG  0.38f                  // CO
#define C_TE  0.30f                  // thermal anomaly
#define C_GG  0.30f                  // gas leak  (severe GAS risk, moderate FIRE risk)

// ------------------------------------------------- corroboration bonuses ---
#define K_FV_GG  25.0f               // fuel + ignition: the worst pair
#define K_FV_SG  12.0f
#define K_SV_SG  10.0f               // cross-modal smoke agreement
#define K_SG_TE   7.0f
#define K_CG_SG   6.0f
#define K_FV_SV   6.0f
#define K_SV_TE   5.0f
#define K_GG_CG   5.0f
#define K_TRIPLE 18.0f               // gas x smoke x flame

// ----------------------------------------------------- context suppression -
#define D_COOK   0.35f
#define D_STEAM  0.25f
#define D_FLOOR  0.55f               // context can never damp below this

// -------------------------------------------------------------- states -----
#define TH_NORMAL_COOKING  10
#define TH_WARNING         25
#define TH_SMOKE_WARNING   45
#define TH_FIRE_SUSPECTED  60
#define TH_FIRE_DETECTED   70        // requires flame confirmation
#define TH_FIRE_NO_VISION  92        // score alone, without visual confirmation
#define TH_CRITICAL        90        // with flame + gas
#define TH_CRITICAL_ALONE  96
#define TH_GAS_SUSPECT     40
#define TH_GAS_DETECTED    70
#define TH_CO_HAZARD       65
#define FLAME_CONFIRM      0.60f

#define HYST_DROP          12        // exit threshold = entry - this
#define DWELL_MIN_MS      20000UL    // minimum time in any WARNING+ state
#define DWELL_ALARM_MS    90000UL    // minimum time in FIRE_DETECTED / CRITICAL
#define ENTRY_FIRE_MS      3000UL
#define ENTRY_GAS_MS       5000UL
#define RECOVER_MS        60000UL
#define MUTE_MS          120000UL

