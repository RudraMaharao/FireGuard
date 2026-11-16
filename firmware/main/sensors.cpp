#include "sensors.h"
#include "alerts.h"
#include "bme280.h"
#include "brain/adapt.h"
#include "brain/crossval.h"
#include "two_sensor_model.h"
#include "ads1115.h"
#include "brain/sensor_features.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "vision_service.h"
#include <time.h>
namespace sensors {
namespace {
SemaphoreHandle_t s_lock;
QueueHandle_t s_commands;
Snapshot s_snapshot;
uint32_t muteUntil = 0, lastDemand = 0;
bool hadDemand = false;
RelativeMQ mq2, mq135;
Ads1115 adc;
float adcMv2=NAN, adcMv135=NAN;
bool adcOK=false;
Bme280 bme;
bool bmeOK = false;
FeatureRing ring;
TwoSensorModel modelA;
// Learned coefficients from the old three-sensor build are not loaded.
CrossVal xval;
float dRH60 = 0.0f;
float rhHist[121];
int rhIdx = 0, rhN = 0; // 60 s of RH at 2 Hz
Fusion fu;
struct {
    bool muted = false;
} ui;
struct {
    uint32_t lastGoodMs = 0;
} rx;
Preferences prefs;

float feat[N_FEAT];
float devScale[3] = {0.05f, 0.05f, 0.05f};
float tC = NAN, rh = NAN, hPa = NAN;
float pBase = NAN, tBase = NAN;
bool calLoaded = false;

VisionOut cam = {};
bool camValid = false;
uint32_t camAgeMs = 0;

TwoSensorScores aScore = {0, 0, 0};

uint8_t label = 0; // data-collection session label 0..15
uint32_t bootMs = 0, lastTick = 0, lastFreezeStart = 0;
bool baselineFrozen = false;

// ---- burn-in accounting and in-place R0 capture ---------------------------
// The burn-in clock lives here as well as in 01_bringup, and both share the
// same NVS key, so hours accumulate no matter which sketch is running. That is
// what lets you run the real system from night one and still capture a valid
// R0 two days later without reflashing anything.
uint32_t burninSec = 0, lastBurninSave = 0;
float burninFrac = 0;
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void loadCal() {
    // Old R0 values were acquired with different hardware; never reuse them.
    mq2.baseInit = mq135.baseInit = false;
    calLoaded = false;
    Serial.println("FireGuard v3.7 snapshot: ADS1115 A0=MQ2, A2=MQ135. CO unavailable; baseline-relative only.");
}
static void showBurnin() {
    Serial.printf("Two-sensor burn-in %.2f h. Baseline-relative prototype; no ppm calibration.\n",burninSec/3600.0f);
}
static void startCapture() {
    // Re-baselining an elevated atmosphere hides it. Only allow after burn-in,
    // in SAFE, with both working channels and a deliberately requested clean-air capture.
    if (burninSec < 48UL*3600 || fu.state != ST_SAFE || !mq2.valid || !mq135.valid) {
        Serial.println("Clean-air baseline reset refused: requires 48 h burn-in, SAFE and valid MQs."); return;
    }
    mq2.baseInit=mq135.baseInit=false;
    Serial.println("Clean-air baseline will be re-seeded. This is not R0/ppm calibration.");
}

#define CSV_HEADER                                                                                           \
    "ms,epoch,dev2,unused_mq7,dev135,signal2,unused_r7,signal135,sc2,unused_sc7,sc135,tC,rh,hPa,"                                          \
    "a_gas,unused_co,a_smoke,m_gas,unused_mco,m_smoke,unused_codose,dT60,dAH60,dRH60,"                                        \
    "cnn_flame,cnn_burner,cnn_smkL,cnn_smkH,cnn_person,cnn_pconf,cnn_motion,"                                \
    "cnn_conf,cnn_luma,vis_credit,hint,"                                                                     \
    "c_fv,c_sg,c_sv,c_cg,c_te,c_gg,w_fv,w_sg,w_sv,w_cg,w_te,w_gg,"                                           \
    "base,bonus,damp,frs,grs,unused_crs,state,flags,label"

void logRow(const Snapshot &sp) {
    const auto &vo = sp.vision;
    char b[700];
    snprintf(b, sizeof b,
             "%lu,%ld,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.2f,%.1f,%.1f,"
             "%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.0f,%.3f,%.3f,%.2f,"
             "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.0f,%.3f,%u,"
             "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
             "%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%u,%u,%u\n",
             (unsigned long)millis(), (long)time(nullptr), sp.dev[0], sp.dev[1], sp.dev[2], sp.ratio[0],
             sp.ratio[1], sp.ratio[2], sp.devScale[0], sp.devScale[1], sp.devScale[2], sp.tC, sp.rh, sp.hPa,
             sp.aGas, sp.aCo, sp.aSmoke, sp.mGas, sp.mCo, sp.mSmoke, sp.coDose, sp.dT60, sp.dAH60, sp.dRH60,
             vo.openFlame, vo.burnerFlame, vo.smokeLight, vo.smokeHeavy, vo.person, vo.personConf, vo.motion,
             vo.conf, vo.luma, sp.visionCredit, sp.hint, sp.cond[0], sp.cond[1], sp.cond[2], sp.cond[3],
             sp.cond[4], sp.cond[5], sp.w[0], sp.w[1], sp.w[2], sp.w[3], sp.w[4], sp.w[5], sp.sBase, sp.bonus,
             sp.damp, sp.frs, sp.grs, sp.crs, sp.state, sp.flags, sp.label);
    ::printf(">%s", b);
}
void tick(float dt, uint32_t now) {
    // Local vision is copied atomically; no UART or ESP-NOW transport.
    auto ve = vision_service::evidence();
    cam = ve.output;
    rx.lastGoodMs = ve.at;
    ui.muted = int32_t(muteUntil - now) > 0;
    // ---------------------------------------------------- 1. sample sensors --
    bool read2 = adc.readMv(0, adcMv2), read135 = adc.readMv(2, adcMv135);
    adcOK = read2 && read135;
    bool v2 = mq2.sample(read2 ? adcMv2*2.0f : NAN);
    bool v135 = mq135.sample(read135 ? adcMv135*2.0f : NAN);
    bmeOK = bme.read(tC, rh, hPa);

    // Burn-in accrues whenever the sensors are powered, whichever sketch is running.
    burninFrac += dt;
    if (burninFrac >= 1.0f) {
        uint32_t s = (uint32_t)burninFrac;
        burninSec += s;
        burninFrac -= s;
    }
    if (now - lastBurninSave >= 300000UL) {
        lastBurninSave = now;
        prefs.begin("kfg", false);
        prefs.putUInt("burnin", burninSec);
        prefs.end();
    }

    devScale[0] = mq2.devScale;
    devScale[1] = NAN; // reserved legacy CO column; unavailable, never a healthy zero
    devScale[2] = mq135.devScale;
    float dev[3] = {mq2.dev, NAN, mq135.dev};
    float ah = absHumidity(tC, rh);
    float history[3] = {isfinite(mq2.dev)?mq2.dev:0.0f, 0, isfinite(mq135.dev)?mq135.dev:0.0f};
    ring.push(history, isnan(tC) ? 25.0f : tC, isnan(ah) ? 10.0f : ah);

    if (isnan(pBase))
        pBase = hPa;
    else if (!isnan(hPa))
        pBase += (hPa - pBase) * (dt / 300.0f);
    if (isnan(tBase))
        tBase = tC;

    float warmMin = (now - bootMs) / 60000.0f;
    ring.compute(feat, devScale, isnan(hPa) ? 0.0f : (hPa - pBase), warmMin);

    // ---------------------------------------------------- 2. Model A ---------
    aScore = modelA.run(mq2.dev, mq135.dev, bmeOK?feat[20]:NAN, bmeOK?feat[21]:NAN);

    // ---------------------------------------------------- 3. camera evidence -
    camAgeMs = now - rx.lastGoodMs;
    camValid = (rx.lastGoodMs != 0) && (camAgeMs < 4000UL) && (cam.conf > 0.05f);

    float sv = camValid ? max(0.45f * cam.smokeLight, cam.smokeHeavy) : 0.0f;
    float fv = camValid ? cam.openFlame : 0.0f;

    // ------------------------------------------ 3b. cross-modal validation ---
    // 60-second relative-humidity slope. Absolute humidity is already feature 21;
    // the SIGNS of the two together are what separate a fire from a boiling pot,
    // so both are needed and neither is redundant.
    rhHist[rhIdx] = isnan(rh) ? (rhN ? rhHist[(rhIdx + 120) % 121] : 50.0f) : rh;
    int rhOld = (rhIdx + 1) % 121;
    if (rhN < 121) {
        rhN++;
        rhOld = 0;
    }
    dRH60 = rhHist[rhIdx] - rhHist[rhOld];
    rhIdx = (rhIdx + 1) % 121;

    // ---------------------------------------------------- 4. thermal channel -
    float dT60 = feat[20];
    float te = clamp01(0.60f * (dT60 / 3.0f) + 0.40f * ((tC - tBase) / 8.0f));
    if (isnan(te))
        te = 0;

    // ---------------------------------------------------- 5. fusion inputs ---
    fu.raw[CH_FV] = fv;
    fu.raw[CH_SV] = sv;
    fu.raw[CH_SG] = aScore.smoke;
    fu.raw[CH_CG] = 0; // removed channel has zero fusion weight, not a synthetic sensor input
    fu.raw[CH_GG] = aScore.gas;
    fu.raw[CH_TE] = te;

    bool warming = (now - bootMs) < (uint32_t)WARMUP_S * 1000UL;
    bool mqAlive = v2 && v135;
    float wMq = (!mqAlive || warming) ? 0.0f : 1.0f;
    // MOX humidity cross-sensitivity is real: above 85 %RH a steam plume reads as
    // gas. Derate rather than ignore, so the channel still contributes something.
    if (!isnan(rh) && rh > 85.0f)
        wMq *= 0.6f;

    float wCam = camValid ? clamp01(cam.conf) : 0.0f;
    float wBme = bmeOK ? 1.0f : 0.0f;

    // The eyes only keep full weight when the other senses agree with them.
    // Note which way this runs: an unsupported VISUAL claim is downweighted, a
    // SENSOR claim the camera cannot see is not. A fire behind a cabinet door is
    // invisible and entirely real, so the camera's silence is evidence of nothing.
    xval.update(dT60, feat[21], dRH60, aScore.smoke, aScore.gas, aScore.co, fv, sv, wCam);

    fu.w[CH_FV] = wCam * xval.visionCredit;
    fu.w[CH_SV] = wCam * xval.visionCredit;
    fu.w[CH_SG] = wMq;
    fu.w[CH_CG] = 0;
    fu.w[CH_GG] = wMq;
    fu.w[CH_TE] = wBme;

    // ---------------------------------------------------- 6. context ---------
    // Benign cooking: the camera sees a normal burner, the gas channels are quiet,
    // and the temperature is not running away. No camera => no damping, which is
    // deliberately the conservative direction.
    float gasQuiet = 1.0f - clamp01(aScore.gas / 0.30f);
    
    float tQuiet = 1.0f - clamp01(dT60 / 3.0f);
    fu.kCook = camValid ? clamp01(cam.burnerFlame * gasQuiet * tQuiet) : 0.0f;

    // Steam: absolute humidity surging while MQ-2 stays flat and nothing is alight.
    float z2 = mq2.z;
    float dAH60 = feat[21];
    fu.kSteam = clamp01((dAH60 - 1.5f) / 4.0f) * (1.0f - clamp01(z2 / 2.0f)) * (1.0f - clamp01(fv * 2.0f));

    // ------------------------------------- 6b. adaptation and occupancy ------
    // Both of these are fenced: each returns exactly 1.0 once a channel's
    // evidence is in the hazard band, so neither what the system has learned nor
    // who it can see is able to attenuate a real hazard. fusion.h floors the
    // score against the unmodified evidence as well, in case they compound.
    fu.condition(dt);
    {
        float pc = camValid ? cam.personConf : 0.0f;
        float pv = camValid ? cam.person : 1.0f;
        for (int i = 0; i < N_CHAN; i++) {
            float ev = fu.cond[i];
            fu.ctx[i] = CrossVal::personFactor(pv, pc, ev);
        }
        fu.dwellScale = CrossVal::entryDwellScale(pv, pc);
    }

    // ---------------------------------------------------- 7. run fusion ------
    fu.aggregate(dt);

    bool lpgSig = false; // Two broad MQ channels cannot identify LPG selectively.
    fu.auxScores(lpgSig);

    bool anyInvalid = (baselineFrozen && now - lastFreezeStart > BASE_FREEZE_MAX * 1000UL) || !mqAlive ||
                      !bmeOK || (hadDemand && now-lastDemand < 120000UL && (!rx.lastGoodMs || camAgeMs > 10000UL));
    fu.updateState(now, warming, anyInvalid);

    // ---------------------------------------------------- 8. baseline rule ---
    // The baseline must not chase a slow leak. It updates ONLY when the system is
    // quiet; if it has been frozen for more than six hours the reference is too
    // old to trust and we say so rather than pretending.
    bool quiet = (fu.state == ST_SAFE) && (fabsf(mq2.z) < BASE_FREEZE_Z) &&
                 (fabsf(mq135.z) < BASE_FREEZE_Z);
    if (quiet) {
        baselineFrozen = false;
    } else if (!baselineFrozen) {
        baselineFrozen = true;
        lastFreezeStart = now;
    }
    if (baselineFrozen && (now - lastFreezeStart) > BASE_FREEZE_MAX * 1000UL)
        fu.sensorError = true;

    mq2.updateBaseline(dt, quiet);
    mq135.updateBaseline(dt, quiet);
    if (quiet && !isnan(tC))
        tBase += (tC - tBase) * (dt / BASE_TAU_S);

    // ---------------------------------------------------- 9. tell the S3 -----
    Snapshot sp;
    sp.ms = now;
    for (int i = 0; i < 3; i++) {
        sp.dev[i] = dev[i];
        sp.devScale[i] = devScale[i];
    }
    sp.ratio[0] = mq2.ratio;
    sp.ratio[1] = NAN;
    sp.ratio[2] = mq135.ratio;
    sp.tC = tC;
    sp.rh = rh;
    sp.hPa = hPa;
    sp.hint = xval.hint;
    sp.dT60 = dT60;
    sp.dAH60 = feat[21];
    sp.dRH60 = dRH60;
    sp.aGas = aScore.gas;
    sp.aCo = NAN;
    sp.aSmoke = aScore.smoke;
    sp.mGas = aScore.mGas;
    sp.mCo = NAN;
    sp.mSmoke = aScore.mSmoke;
    sp.coDose = NAN;
    sp.visionCredit = xval.visionCredit;
    for (int i = 0; i < 6; i++) {
        sp.cond[i] = fu.cond[i];
        sp.w[i] = fu.w[i];
    }
    sp.sBase = fu.sBase;
    sp.bonus = fu.bonus;
    sp.damp = fu.damp;
    sp.frs = fu.frs;
    sp.grs = fu.grs;
    sp.crs = NAN;
    sp.state = (uint8_t)fu.state;
    sp.label = (uint8_t)(label & 0x0F);
    sp.flags = (fu.sensorError ? 1 : 0) | (warming ? 2 : 0) | (fu.rateEscalated ? 4 : 0) |
               (ui.muted ? 8 : 0) | (xval.outOfView > 0.25f ? 16 : 0) | (aScore.steamVetoed ? 32 : 0);
    bool request = !warming && (xval.hint >= CrossVal::HINT_WATCH || fu.frs >= 25.0f);
    if (request) {
        lastDemand = now;
        hadDemand = true;
    }
    sp.cameraRequested = hadDemand && (request || now - lastDemand < 120000UL);
    sp.cameraValid = camValid;
    sp.visionAgeMs = camAgeMs;
    sp.vision = cam;
    sp.muted = ui.muted;
    sp.calibrated = false;
    sp.adcOK = adcOK; sp.mq2OK=v2; sp.mq135OK=v135;
    sp.adcMv2=adcMv2; sp.adcMv135=adcMv135;
    sp.burninSec = burninSec;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot = sp;
    xSemaphoreGive(s_lock);
    alerts::observe(sp);
    logRow(sp);
}

void run(void *) {
    loadCal();
    modelA.begin();

    prefs.begin("kfg", true);
    burninSec = prefs.getUInt("burnin", 0);
    prefs.end();
    for (auto &v : rhHist)
        v = 50.0f;
    bootMs = lastBurninSave = millis();
    fu.begin(bootMs);
    lastTick = bootMs;
    puts(">" CSV_HEADER);
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = millis();
        Command c;
        while (xQueueReceive(s_commands, &c, 0) == pdTRUE) {
            switch (c) {
            case MUTE:

                muteUntil = now + MUTE_MS;
                break;
            case CYCLE_LABEL:
                label = (label + 1) % 10;
                break;
            case CAPTURE_R0:
                startCapture();
                break;
            case PRINT_PHYSICS:
                modelA.print(Serial, aScore);
                break;
            case PRINT_ADAPT:
                Serial.println("Adaptive nuisance suppression disabled for revision 2.");
                break;
            case RESET_ADAPT:
                Serial.println("No revision-2 adaptive state to reset.");
                break;
            case PRINT_CROSSVAL:
                xval.print(Serial);
                break;
            case PRINT_BURNIN:
                showBurnin();
                break;
            case PRINT_STATUS:
                Serial.printf("%s FRS %.1f Gas %.1f CO unavailable label %u\n", fu.stateName(), fu.frs, fu.grs,
                              label);
                break;
            }
        }
        float dt = (now - lastTick) / 1000.0f;
        lastTick = now;
        tick(dt, now);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
    }
}
} // namespace
Snapshot snapshot() {
    Snapshot s;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s = s_snapshot;
    xSemaphoreGive(s_lock);
    return s;
}
bool command(Command c) { return s_commands && xQueueSend(s_commands, &c, 0) == pdTRUE; }
void start(i2c_master_bus_handle_t bus) {
    s_lock = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(12, sizeof(Command));
    configASSERT(s_lock && s_commands);
    adc.begin(bus);
    bme.begin(bus);
    configASSERT(xTaskCreatePinnedToCore(run, "sensors", 8192, nullptr, 8, nullptr, 0) == pdPASS);
}
} // namespace sensors
