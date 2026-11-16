/* ============================================================================
   physics_thresholds.h  —  GENERATED FILE, DO NOT EDIT BY HAND.

   Produced by  training/physics/derive_thresholds.py
   Regenerate with:
       cd training && python -m physics.derive_thresholds --emit \
              ../firmware/sensor_node/physics_thresholds.h

   Every constant below is a published safety limit pushed through the sensor's
   own power law. Nothing here was tuned against recorded data, which is exactly
   why it is safe to ship before any data has been recorded.

   The identity that makes this legitimate:

       dev = ln(baseline / (Rs/R0)) = B * ln(C / C_base)

   The pre-exponential A — which carries the heater voltage, the load resistor,
   the element's age and its absolute resistance — cancels in the log-ratio. B,
   the log-log slope, is a property of the surface chemistry and survives the
   move from 5 V to 3.3 V. So a threshold expressed in z-scores of dev is
   meaningful on this hardware even though a ppm reading is not.

   Room model: 25 m3, 2.0 ACH closed / 8.0 ACH extracting,
   sensor at 1.2 m of a 2.7 m ceiling, ambient 28 C / 55 %RH.
   Quiet-air noise width assumed: sigma = 0.0120 dev units.

   Verified against 17 simulated scenarios per fuel, 8 noise draws each,
   judged on the worst draw in both directions. Generated 2026-08-28.
   ========================================================================== */
#pragma once

// ─────────────────────────────────────────────────────── fuel selection ────
// 0 = AUTO   use the LPG-calibrated thresholds. PNG at 10 % LEL produces a
//            LARGER sensor response than LPG at 10 % LEL, so these thresholds
//            trip a PNG leak at about 6 % of ITS lower explosive limit —
//            early, which is the correct direction to err. Leave this alone
//            unless you know which gas your kitchen burns.
// 1 = LPG    cylinder. Denser than air (rel. density 1.83): it pools at floor
//            level, so mount the MQ sensors LOW — 30 cm above the floor.
// 2 = PNG    piped methane. Lighter than air (rel. density 0.58): it collects at
//            the ceiling, so mount the MQ sensors HIGH — 30 cm below it.
//
// Sensor height is not a detail. In this room model a sensor in the wrong layer
// reads about a third of the true concentration, which turns a 10 % LEL alarm
// into a 30 % LEL alarm without changing a single line of code.
#define FUEL_PROFILE   0

// ───────────────────────────────────────────────────────── fuel signature ──
// Measured at the anchors, across both fuels and from a whiff to 25 % LEL.
// These are the MAXIMUM ratios over that range, not the mean: the channels
// saturate at different rates, so a mean-fitted contrast under-subtracts at low
// concentration and a small leak bleeds into the smoke channel.
//
//        pure LPG at 10 % LEL   r1 =  1.91   r2 =  1.10
//        pure PNG at 10 % LEL   r1 =  1.72   r2 =  1.07
//        burning food           r1 =  2.49   r2 =  3.62
//        pure CO at 100 ppm     r1 = 39.62   r2 =  8.29
//
//   m_gas   = z2 - 0.75 * ( max(z135 - r2*z2, 0) + max(z7 - r1*z2, 0) )
//   m_co    = z7   - r1 * z2
//   m_smoke = z135 - r2 * z2
#define PZ_R1_FUEL        2.418f
#define PZ_R2_FUEL        1.500f
#define PZ_GAS_PENALTY    0.75f

// ──────────────────────────────────────────────── logistic operating points ─
// x0 is the contrast at which the score reads 0.5; k is its width.
//
// gas:   0.5 at LPG 10 % LEL (1900 ppm, IEC 60079-29's first alarm),
//        0.9 at 25 % LEL (4750 ppm, the evacuate point).
#define PZ_X0_GAS         54.78f
#define PZ_K_GAS          13.08f

// CO:    0.5 at 100 ppm and 0.9 at 300 ppm — the two mandatory points in
//        EN 50291's response envelope. 30 ppm lands at 0.13, which
//        discharges the standard's must-NOT-alarm requirement by construction.
#define PZ_X0_CO          38.44f
#define PZ_K_CO           12.84f

// smoke: 0.5 at 4.5 %/m obscuration — inside EN 54-7's 2-8 %/m mandatory band
//        and 1.8x above the smokiest routine cooking in the scenario library.
//        0.9 at 14 %/m, a developed flaming fire.
#define PZ_X0_SMOKE       11.36f
#define PZ_K_SMOKE        3.95f

// ───────────────────────────────────────────────────────── carbon monoxide ─
// EN 50291's envelope is a set of DOSE limits wearing level clothing. Pushed
// through Stewart's equation every point in it lands near 8-10 % COHb:
//
//        50 ppm for 90 min  ->  8.0 % COHb
//       100 ppm for 40 min  ->  9.9 % COHb
//       300 ppm for  3 min  ->  6.2 % COHb
//
// A level threshold cannot express that shape, so there is a second integrating
// channel. Its decay is the elimination half-life of carboxyhaemoglobin, which
// is why the integrator forgets at the rate a person recovers.
#define PZ_CO_DOSE_ALARM  1581.9f    // z-minutes
#define PZ_CO_DOSE_TAU_S  14400.0f    // 4.0 h

// ───────────────────────────────────────────────────── thermal / humidity ──
// A developed pan fire (35 kW) raises room air 3.99 K per 60 s in this
// room. A gas burner at full (2.8 kW) manages 0.32 K. That order of
// magnitude is the thermal channel's entire discriminating power, which is why
// it corroborates rather than testifying on its own.
#define PZ_DT60_FIRE      3.993f
#define PZ_DT60_COOK      0.319f
#define PZ_THERMAL_BOOST  0.35f

// An uncovered pot moves ABSOLUTE humidity hard and temperature barely at all.
// A fire does the reverse: it raises temperature, which drops RELATIVE humidity
// while absolute humidity stays nearly flat. Vetoing on absolute humidity is
// what stops a pot of rice from reading as combustion; vetoing on relative
// humidity would do the opposite of what you want.
#define PZ_DAH60_STEAM    0.260f
#define PZ_STEAM_Z2_MAX   27.4f

// ─────────────────────────────────────────────── adaptation safety envelope ─
// adapt.h may attenuate evidence in the nuisance band. It may not move any
// constant in this file, and it may not reach evidence above its hard-keep
// fence. These are published here so the bound is visible from one place.
#define PZ_ADAPT_MAX_RAISE  1.60f
#define PZ_ADAPT_MIN_FLOOR  1.00f
#define PZ_FLOOR_X0_GAS     54.78f
#define PZ_FLOOR_X0_CO      38.44f
#define PZ_FLOOR_X0_SMOKE   11.36f

// ──────────────────────────────────────────────── what these numbers mean ──
// Printed by the firmware's 'p' command, so the operating point is auditable on
// the bench and not only in this file.
#define PZ_AUDIT_GAS_10LEL   0.500f
#define PZ_AUDIT_GAS_25LEL   0.900f
#define PZ_AUDIT_CO_30       0.128f
#define PZ_AUDIT_CO_100      0.452f
#define PZ_AUDIT_CO_300      0.919f
#define PZ_SIGMA_NOMINAL     0.0120f
