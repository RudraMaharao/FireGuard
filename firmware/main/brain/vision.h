/* ============================================================================
   vision.h — Model B.

   Ships with a CLASSICAL DETECTOR so the camera channel works the day you build
   it, with no dataset and no training. It is not a CNN and does not pretend to
   be: it is colour masking plus TEMPORAL FLICKER plus contrast loss. Flicker is
   what makes it more than a colour threshold — a real flame's mask changes
   shape 10-20 % per frame, a red T-shirt's does not.

   The four outputs are exactly the four heads the CNN will produce, so when you
   train it (training/train_cnn.py) you swap the implementation and nothing else
   in the system changes.

     openFlame    uncontained flame   -> drives fire risk
     burnerFlame  normal stove flame  -> drives COOKING CONTEXT, not risk
     smokeLight   visible haze
     smokeHeavy   dense smoke
     person       is anyone here      -> ABSENCE escalates; presence never
                                         suppresses above the hazard fence

   THE FIFTH HEAD, AND WHY THE CLASSICAL VERSION IS HONEST ABOUT ITSELF.

   Whether a person is present changes what smoke MEANS. Smoke with someone
   standing at the hob is a cooking event that a human is already handling.
   The same smoke in an empty kitchen is the beginning of an unattended fire,
   and the alarm is the only thing that is going to happen next.

   A CNN can answer "is there a person in frame" properly. The classical
   detector cannot, and it does not pretend to. What it CAN do is answer a
   weaker question that turns out to carry most of the value: has anything in
   this room moved in the last few minutes? A kitchen with a person in it is
   never still. So the classical path reports occupancy from sustained motion
   and caps its own confidence at PERSON_CONF_CLASSICAL, which is below the
   threshold at which presence is allowed to damp anything — but above the
   threshold at which ABSENCE is allowed to escalate.

   That asymmetry falls out of the confidence value alone, with no special
   casing: weak evidence of absence is still worth acting on, because acting on
   it means alarming sooner. Weak evidence of presence is worth nothing,
   because acting on it would mean alarming later.
   ========================================================================== */
#pragma once
#include <Arduino.h>
#include <math.h>

#define USE_TRAINED_CNN 0        // <-- set to 1 after training

// jpg2rgb565() writes native-endian uint16 words on the ESP32, but a few
// esp32-camera versions differ. If the serial "colour check" in vision_node.ino
// reports that a red object reads as blue, set this to 1.
#define RGB565_BYTE_SWAP 0

#define VW 160                   // analysis width  (QVGA / 2)
#define VH 120                   // analysis height
#define VN (VW * VH)
#define BLK 8                    // flicker block size
#define BW (VW / BLK)            // 20
#define BH (VH / BLK)            // 15

// The classical occupancy proxy never claims more confidence than this. It is
// deliberately below CrossVal::personFactor's 0.35 gate, so until the CNN is
// trained, PRESENCE cannot damp anything at all — while ABSENCE, which is
// evaluated by entryDwellScale on the same number, still shortens the dwell.
#define PERSON_CONF_CLASSICAL   0.30f
#define OCCUPANCY_MOTION_ON     0.020f   // frame-change fraction that counts
#define OCCUPANCY_HOLD_S        180.0f   // still for 3 min -> nobody here

struct VisionOut {
  float openFlame = 0, burnerFlame = 0, smokeLight = 0, smokeHeavy = 0;
  float person = 0, personConf = 0;
  float conf = 0, luma = 0, motion = 0;
  bool  sceneChanged = false, lensBlocked = false, tooDark = false;
};

class Vision {
public:
  uint8_t *flameMask = nullptr;    // VN bytes
  uint8_t *prevMask  = nullptr;
  uint8_t *lumaPrev  = nullptr;    // for the motion / occupancy estimate
  float    edgeRef   = -1;
  uint32_t frames    = 0;
  uint32_t lastHash  = 0;
  float    stillFor  = 0;          // seconds since the last real motion
  float    dtFrame   = 0.5f;       // set by the caller from the actual interval

  bool begin() {
    flameMask = (uint8_t *)ps_malloc(VN);
    prevMask  = (uint8_t *)ps_malloc(VN);
    lumaPrev  = (uint8_t *)ps_malloc(VN);
    if (!flameMask || !prevMask || !lumaPrev) return false;
    memset(flameMask, 0, VN);
    memset(prevMask, 0, VN);
    memset(lumaPrev, 0, VN);
    return true;
  }

  /* rgb565 buffer, VW x VH, as produced by jpg2rgb565(). */
  VisionOut analyse(const uint8_t *rgb8) {
    const uint16_t *rgb = (const uint16_t *)rgb8;
    VisionOut o;

    uint32_t flamePx = 0, bluePx = 0, smokePx = 0, motionPx = 0;
    double   lumaSum = 0, edgeSum = 0;
    uint32_t hash = 0;
    int      flameCx = 0, flameCy = 0;

    // ---------------- pass 1: per-pixel masks + luminance -------------------
    for (int y = 0; y < VH; y++) {
      for (int x = 0; x < VW; x++) {
        int i = y * VW + x;
        uint16_t p = px(rgb, i);
        int r = ((p >> 11) & 0x1F) << 3;
        int g = ((p >> 5)  & 0x3F) << 2;
        int b = ( p        & 0x1F) << 3;

        int luma = (r * 77 + g * 150 + b * 29) >> 8;
        lumaSum += luma;
        if (abs(luma - (int)lumaPrev[i]) > 12) motionPx++;
        lumaPrev[i] = (uint8_t)luma;
        if ((i & 0x3FF) == 0) hash = hash * 31u + (uint32_t)luma;

        // Flame: bright, red-dominant, with a green component (orange, not pure red)
        uint8_t isFlame = (r > 150 && r > (g * 13) / 10 && g > (b * 21) / 20 && g > 55) ? 1 : 0;
        flameMask[i] = isFlame;
        if (isFlame) { flamePx++; flameCx += x; flameCy += y; }

        // Blue burner flame: distinctive, and rarely confused with anything else
        if (b > 90 && b > (r * 23) / 20 && b > (g * 21) / 20) bluePx++;

        // Smoke: desaturated mid-tones
        int mx = max(r, max(g, b)), mn = min(r, min(g, b));
        int sat = mx > 0 ? (255 * (mx - mn)) / mx : 0;
        if (sat < 46 && luma > 60 && luma < 205) smokePx++;
      }
    }
    o.luma = lumaSum / VN;

    // ---------------- pass 2: edge energy (Sobel-lite on luma) --------------
    for (int y = 1; y < VH - 1; y += 2) {
      for (int x = 1; x < VW - 1; x += 2) {
        edgeSum += fabsf((float)lum(rgb, x + 1, y) - lum(rgb, x - 1, y))
                 + fabsf((float)lum(rgb, x, y + 1) - lum(rgb, x, y - 1));
      }
    }
    float edgeNow = (float)(edgeSum / ((VW / 2) * (VH / 2)));

    // ---------------- flicker: the real flame discriminator -----------------
    uint32_t changed = 0;
    for (int i = 0; i < VN; i++) if (flameMask[i] != prevMask[i]) changed++;
    float flicker = flamePx > 20 ? (float)changed / (float)flamePx : 0.0f;
    memcpy(prevMask, flameMask, VN);

    float flameFrac = (float)flamePx / VN;
    float blueFrac  = (float)bluePx  / VN;
    float smokeFrac = (float)smokePx / VN;

    // ---------------- contrast loss vs a slow reference ---------------------
    // Smoke veils the scene, so edge energy collapses. The reference updates
    // only when nothing smoke-like is present, for the same reason the gas
    // baseline freezes: otherwise it adapts to the hazard and goes blind.
    float edgeDrop = 0;
    if (edgeRef > 0.5f) edgeDrop = constrain(1.0f - edgeNow / edgeRef, 0.0f, 1.0f);

    // ---------------- scores ------------------------------------------------
    // Openness: a burner flame is small, low in frame and blue-adjacent.
    float openness = 1.0f;
    if (flamePx > 0) {
      float cy = (float)flameCy / flamePx / VH;         // 0 = top, 1 = bottom
      openness *= constrain(1.3f - cy, 0.25f, 1.0f);    // low in frame -> likely burner
    }
    if (blueFrac > 0.004f) openness *= 0.45f;           // blue flame nearby -> stove

    float flameSize   = sat01(flameFrac, 0.004f, 0.10f);
    float flickerGate = sat01(flicker,  0.06f,  0.30f);
    o.openFlame   = constrain(flameSize * (0.25f + 0.75f * flickerGate) * openness, 0.0f, 1.0f);
    o.burnerFlame = constrain(sat01(blueFrac, 0.002f, 0.03f), 0.0f, 1.0f);

    float smokeRaw = 0.55f * sat01(smokeFrac, 0.15f, 0.70f) + 0.45f * edgeDrop;
    o.smokeLight = constrain(sat01(smokeRaw, 0.18f, 0.55f), 0.0f, 1.0f);
    o.smokeHeavy = constrain(sat01(smokeRaw, 0.50f, 0.85f), 0.0f, 1.0f);

    // ---------------- confidence -------------------------------------------
    // A camera that cannot see is not a camera that sees nothing.
    float c = 1.0f;
    if (o.luma < 25 || o.luma > 230) c = 0.0f;                    // dark or blown out
    else if (o.luma < 45 || o.luma > 210) c *= 0.4f;
    if (edgeNow < 3.0f) c *= 0.2f;                                // blocked or defocused
    if (hash == lastHash && frames > 3) c = 0.0f;                 // frozen sensor
    if (frames < 10) c *= 0.3f;                                   // still settling
    lastHash = hash;
    o.conf = constrain(c, 0.0f, 1.0f);

    // ---------------- occupancy ---------------------------------------------
    // A kitchen with a person in it is never completely still for three
    // minutes. This is a weak signal and it is labelled as one; its whole job
    // is to let an unattended hazard escalate faster than an attended one.
    o.motion = (float)motionPx / VN;
    if (o.motion > OCCUPANCY_MOTION_ON && o.conf > 0.4f) stillFor = 0.0f;
    else stillFor += dtFrame;
    o.person = constrain(1.0f - stillFor / OCCUPANCY_HOLD_S, 0.0f, 1.0f);
    // Confidence collapses with the camera's own confidence: an unusable frame
    // is not evidence that the room is empty, it is evidence of nothing.
    o.personConf = (o.conf > 0.4f) ? PERSON_CONF_CLASSICAL * o.conf : 0.0f;

    o.tooDark     = (o.luma < 45.0f);
    o.lensBlocked = (edgeNow < 3.0f);
    // A large, sustained edge-energy shift with no flame or smoke behind it
    // means the camera moved or the scene was rearranged, which invalidates the
    // reference the smoke detector measures contrast loss against.
    o.sceneChanged = (edgeRef > 0.5f) &&
                     (fabsf(edgeNow - edgeRef) / edgeRef > 0.55f) &&
                     (smokeRaw < 0.15f);

    if (edgeRef < 0) edgeRef = edgeNow;
    else if (smokeRaw < 0.15f && o.conf > 0.5f)
      edgeRef += (edgeNow - edgeRef) * 0.01f;                     // ~50 s time constant

    frames++;
    return o;
  }

public:
  static inline uint16_t px(const uint16_t *rgb, int i) {
#if RGB565_BYTE_SWAP
    uint16_t v = rgb[i]; return (uint16_t)((v >> 8) | (v << 8));
#else
    return rgb[i];
#endif
  }

private:
  static inline int lum(const uint16_t *rgb, int x, int y) {
    uint16_t p = px(rgb, y * VW + x);
    int r = ((p >> 11) & 0x1F) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
    return (r * 77 + g * 150 + b * 29) >> 8;
  }
  static inline float sat01(float v, float lo, float hi) {
    if (v <= lo) return 0.0f;
    if (v >= hi) return 1.0f;
    return (v - lo) / (hi - lo);
  }
};

// Five-frame median smoothing on each head, so a single odd frame cannot move
// the fusion engine.
class VisionSmoother {
public:
  VisionOut push(const VisionOut &in) {
    h[idx] = in; idx = (idx + 1) % 5; if (n < 5) n++;
    VisionOut o = in;
    o.openFlame   = med([](const VisionOut &v){ return v.openFlame; });
    o.burnerFlame = med([](const VisionOut &v){ return v.burnerFlame; });
    o.smokeLight  = med([](const VisionOut &v){ return v.smokeLight; });
    o.smokeHeavy  = med([](const VisionOut &v){ return v.smokeHeavy; });
    o.person      = med([](const VisionOut &v){ return v.person; });
    return o;
  }
private:
  VisionOut h[5]; int idx = 0, n = 0;
  template <typename F> float med(F get) {
    float a[5]; int m = n ? n : 1;
    for (int i = 0; i < m; i++) a[i] = get(h[i]);
    for (int i = 0; i < m; i++)
      for (int j = i + 1; j < m; j++) if (a[j] < a[i]) { float t = a[i]; a[i] = a[j]; a[j] = t; }
    return a[m / 2];
  }
};
