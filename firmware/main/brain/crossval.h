/* ============================================================================
   crossval.h — corroboration between the eyes and the other senses.

   A camera is a very good and very gullible witness. It sees an orange, moving,
   bright thing and calls it fire, and it is right most of the time and
   catastrophically wrong the rest — a sunset through a window, a phone torch, a
   pan of tomatoes, the red standby LED on a microwave.

   What a camera cannot do is feel. Fire is not only a visual event: it is
   exothermic, and it is a chemical reaction with products. If something looks
   like flame and the air is not warming and nothing is oxidising, the eyes are
   being fooled. That is the whole idea here, and it is the reason the BME280 —
   the cheapest and least glamorous part in the build — is what makes the camera
   trustworthy.

   THE ASYMMETRY IS THE SAFETY ARGUMENT. Corroboration runs in one direction
   only:

     vision claims, senses silent   ->  DOWNWEIGHT the vision channel.
                                        An unsupported visual claim is probably
                                        an artefact.

     senses claim, vision silent    ->  DO NOT DOWNWEIGHT the sensors.
                                        A smouldering fire inside a cabinet, or
                                        behind the camera, or under a cloth, is
                                        invisible and entirely real. The camera's
                                        silence is evidence of nothing.

   Getting that backwards would build a system that goes quiet exactly when the
   fire is somewhere the camera cannot see, which is where fires usually start.

   The physical discriminators, in order of how much work they do:

     THERMAL.  A flame worth alarming about releases heat. PZ_DT60_FIRE and
     PZ_DT60_COOK come out of the room model and are an order of magnitude
     apart, so "is the air warming faster than a gas ring can warm it" is a
     sharp question with a real answer.

     HUMIDITY INVERSION.  This one is subtle and it is the best single test in
     the system. Boiling water raises ABSOLUTE humidity a lot and temperature a
     little. A fire raises temperature a lot and absolute humidity barely at all
     — combustion does make water, but only about 10 g for a 5 kW minute, versus
     100 g from an uncovered pot. Because saturation pressure climbs steeply
     with temperature, a fire therefore makes RELATIVE humidity FALL while
     absolute humidity stays flat. Steam makes both rise. Nothing else in a
     kitchen does the first thing, which is why steam and smoke — which look
     identical to a camera and nearly identical to a gas sensor — separate
     cleanly here.

     CHEMISTRY.  Combustion products. Slower and less specific than the other
     two, but it is the one that works when the camera is blind.
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <math.h>
#include "physics_thresholds.h"

struct CrossVal {
  // --- outputs ------------------------------------------------------------
  float visionCredit = 1.0f;   // multiplies the vision channels' confidence
  float thermalSupport = 0.0f; // 0..1, how much the air agrees with "fire"
  float steamLikelihood = 0.0f;// 0..1, how much it agrees with "boiling water"
  float outOfView = 0.0f;      // 0..1, senses say yes and the camera says no
  uint8_t hint = 0;            // what to ask the camera to do next

  // What the sensor node asks the vision node for. The camera is not a passive
  // input; when the nose smells something the eyes should be told to look
  // harder, and looking harder costs power that is not worth spending all day.
  enum Hint : uint8_t { HINT_IDLE = 0, HINT_WATCH = 1, HINT_ALERT = 2, HINT_CONFIRM = 3 };

  /* ---------------------------------------------------------------------
     dT60   temperature change over the last 60 s, K
     dAH60  absolute humidity change over the last 60 s, g/m^3
     dRH60  relative humidity change over the last 60 s, %
     aSmoke, aGas, aCo   Model A outputs, 0..1
     vFlame, vSmoke      CNN heads, 0..1
     vConf               camera confidence: 0 when dark, blocked or stale
     --------------------------------------------------------------------- */
  void update(float dT60, float dAH60, float dRH60,
              float aSmoke, float aGas, float aCo,
              float vFlame, float vSmoke, float vConf) {

    // ---- 1. does the air agree that something is burning? ----------------
    // Scaled against what a gas ring can do, not against what a fire can do:
    // the question is "hotter than cooking", and PZ_DT60_COOK is that bar.
    float warmBar = fmaxf(PZ_DT60_COOK, 0.05f);
    thermalSupport = constrain(dT60 / (2.0f * warmBar), 0.0f, 1.0f);

    // ---- 2. the humidity inversion ---------------------------------------
    // Steam: absolute humidity climbing. Fire: temperature climbing while
    // absolute humidity does not, which drags relative humidity down.
    float ahRise = constrain(dAH60 / fmaxf(PZ_DAH60_STEAM, 0.01f), 0.0f, 2.0f);
    float rhFall = constrain(-dRH60 / 3.0f, 0.0f, 1.0f);
    steamLikelihood = constrain(ahRise * (1.0f - 0.7f * rhFall), 0.0f, 1.0f);

    // A dry heat signature: warming, relative humidity falling, absolute
    // humidity flat. This is the fire fingerprint and it is worth as much as
    // the raw temperature slope.
    float dryHeat = thermalSupport * rhFall * (1.0f - constrain(ahRise, 0.0f, 1.0f));

    // ---- 3. chemical corroboration ---------------------------------------
    float chem = fmaxf(aSmoke, fmaxf(aGas, aCo));

    // ---- 4. is the visual claim supported? -------------------------------
    // Only asked when the camera is actually claiming something. A quiet camera
    // needs no corroboration and keeps full credit.
    float claim = fmaxf(vFlame, vSmoke) * vConf;
    if (claim > 0.25f) {
      float support = 1.0f - (1.0f - thermalSupport) * (1.0f - chem) * (1.0f - dryHeat);
      // Floor at VIS_CREDIT_MIN, never zero. A tea light across the room is a
      // real flame that genuinely moves neither the thermometer nor the gas
      // sensors, and the camera is the only instrument that can see it. The
      // floor is what stops corroboration from becoming censorship.
      visionCredit = VIS_CREDIT_MIN + (1.0f - VIS_CREDIT_MIN) * support;
      // Steam actively argues against a visual smoke call: a plume the camera
      // reads as smoke, arriving with a humidity surge, is a pot of rice.
      visionCredit *= (1.0f - 0.45f * steamLikelihood * (vSmoke > vFlame ? 1.0f : 0.3f));
    } else {
      visionCredit = 1.0f;
    }
    visionCredit = constrain(visionCredit, VIS_CREDIT_MIN, 1.0f);

    // ---- 5. the asymmetric half ------------------------------------------
    // Sensors say something, camera does not, and the camera is not blind.
    // This is NOT used to reduce anything. It is published so the UI can say
    // "out of view" and so the state machine knows the visual confirmation gate
    // is unavailable and must not be waited on.
    float senseSays = fmaxf(chem, thermalSupport);
    outOfView = (vConf > 0.4f) ? constrain(senseSays - claim, 0.0f, 1.0f) : 0.0f;

    // ---- 6. what to ask the camera for -----------------------------------
    if (senseSays > 0.55f || claim > 0.55f)      hint = HINT_CONFIRM;
    else if (senseSays > 0.30f || claim > 0.30f) hint = HINT_ALERT;
    else if (senseSays > 0.12f)                  hint = HINT_WATCH;
    else                                          hint = HINT_IDLE;
  }

  /* ---------------------------------------------------------------------
     Person presence, wired asymmetrically.

     ABSENCE ESCALATES. An unattended kitchen with smoke in it is a different
     event from an attended one. Nobody is going to turn the burner off, notice
     the smell, or move the pan. The alarm is the only thing that will happen,
     so it should happen sooner and at a lower bar.

     PRESENCE NEVER SUPPRESSES ABOVE THE FENCE. Someone standing at the hob is
     weak evidence that a small amount of smoke is under control. It is no
     evidence at all that a developing fire is under control, and a monitor that
     stays quiet because it can see you is worse than no monitor — you are the
     person it would be warning.

     Returns a multiplier on the nuisance band only.
     --------------------------------------------------------------------- */
  static float personFactor(float person, float personConf, float evidence) {
    if (personConf < 0.35f) return 1.0f;             // can't tell: change nothing
    if (evidence >= PERSON_FENCE) return 1.0f;       // hazard band: untouchable

    float below = 1.0f - evidence / PERSON_FENCE;
    float p = constrain(person, 0.0f, 1.0f) * constrain(personConf, 0.0f, 1.0f);
    // present -> damp the nuisance band a little; absent -> lift it
    float f = 1.0f - PERSON_DAMP * p * below + PERSON_LIFT * (1.0f - p) * below;
    return constrain(f, 1.0f - PERSON_DAMP, 1.0f + PERSON_LIFT);
  }

  // How much sooner an unattended hazard is allowed to latch, as a fraction of
  // the normal entry dwell. Never applied to the CRITICAL path, which has no
  // dwell to shorten.
  static float entryDwellScale(float person, float personConf) {
    if (personConf < 0.35f) return 1.0f;
    float absent = 1.0f - constrain(person, 0.0f, 1.0f);
    return 1.0f - 0.45f * absent * constrain(personConf, 0.0f, 1.0f);
  }

  static constexpr float VIS_CREDIT_MIN = 0.40f;
  static constexpr float PERSON_FENCE   = 0.55f;   // same fence adapt.h uses
  static constexpr float PERSON_DAMP    = 0.20f;
  static constexpr float PERSON_LIFT    = 0.30f;

  void print(Stream &s) const {
    static const char* H[] = { "idle", "watch", "alert", "confirm" };
    s.printf("  vision credit %.2f  thermal %.2f  steam %.2f  outOfView %.2f  hint %s\n",
             visionCredit, thermalSupport, steamLikelihood, outOfView, H[hint & 3]);
  }
};
