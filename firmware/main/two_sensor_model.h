#pragma once
#include <Arduino.h>
#include <math.h>
#include "brain/config.h"
// Revision 2: no three-sensor classifier, CO estimate, dose, ppm or gas identification.
// The fixed thresholds below are demonstration heuristics; they require field validation.
// dev is log((5V/AO_baseline-1)/(5V/AO_now-1)). The fixed divider load cancels
// from this relative ratio; no assumed MQ module load resistor or R0 is needed.
struct RelativeMQ {
    float ratio=NAN, baseline=NAN, dev=NAN, devScale=0.05f, z=NAN;
    bool valid=false, baseInit=false;
    bool sample(float aoMv) {
        valid = isfinite(aoMv) && aoMv >= 40 && aoMv <= 4960;
        if (!valid) { ratio=dev=z=NAN; return false; }
        ratio = (5000.0f-aoMv)/aoMv;
        if (!baseInit) { baseline=ratio; baseInit=true; }
        dev=logf(baseline/ratio); z=dev/fmaxf(devScale,0.05f); return true;
    }
    void updateBaseline(float dt, bool allowed) {
        if (!valid || !allowed) return;
        float a=constrain(dt/BASE_TAU_S,0.0f,1.0f);
        baseline+=(ratio-baseline)*a;
        devScale=fmaxf(0.05f,devScale+(fabsf(dev)-devScale)*a);
    }
};
struct TwoSensorScores { float gas=0, smoke=0, co=0, mGas=0, mSmoke=0; bool steamVetoed=false; };
class TwoSensorModel {
 public:
    void begin() {}
    TwoSensorScores run(float d2, float d135, float dT, float dAH) const {
        TwoSensorScores s;
        if (!isfinite(d2) || !isfinite(d135)) return s;
        // Both MQs are broad, overlapping gas sensors. These are relative activity
        // channels, not independent measurements or a selective LPG/smoke classifier.
        s.mGas=fmaxf(d2,d135); s.mSmoke=fminf(d2,d135);
        s.gas=constrain((s.mGas-0.10f)/0.65f,0.0f,1.0f);
        s.smoke=constrain((s.mSmoke-0.08f)/0.50f,0.0f,1.0f);
        if (isfinite(dAH) && dAH>1.5f && d2<0.10f && (!isfinite(dT) || dT<1.0f)) {
            s.gas*=0.6f; s.smoke*=0.45f; s.steamVetoed=true;
        }
        return s;
    }
    void print(Stream &out, const TwoSensorScores &s) const {
        out.printf("Two-sensor prototype: gas activity %.3f, combustion activity %.3f. CO unavailable.\n",s.gas,s.smoke);
    }
};
