"""
derive_thresholds.py — turn published safety limits into firmware constants.

Run:  python -m physics.derive_thresholds --report
      python -m physics.derive_thresholds --emit ../firmware/sensor_node/physics_thresholds.h

What this does, in order:

  1. Anchors. Take the published limit for each hazard (10 % LEL for fuel gas,
     EN 50291's CO envelope, EN 54-7's obscuration band) and compute the z-score
     the sensor produces at that concentration. This is a pure calculation from
     the power law — no fitting, no data.

  2. Signature. No MQ channel is selective, but the RATIOS between channels are.
     Compute r1 = z_mq7/z_mq2 and r2 = z_mq135/z_mq2 for each pure hazard and
     find the decision boundaries between them. This replaces the difference-
     based discriminator, which fails at high concentration because MQ-7's low
     baseline makes it saturate before MQ-2 does.

  3. Fit. Solve for the logistic parameters that put each score at 0.5 on its
     published alarm limit and 0.9 at the escalation limit.

  4. Verify. Run every scenario in the library through the resulting model and
     check that benign peaks sit below hazardous minima with margin, and that
     every severity 4-5 case is detected inside its deadline.

  5. Emit. Write the header, with each constant carrying the physical statement
     that produced it and a hard floor the on-device adaptation may not cross.
"""
from __future__ import annotations
import argparse, json, math, sys
from dataclasses import dataclass, asdict
import numpy as np

from . import gas_data as G
from . import room as R
from . import sensors as S
from . import scenarios as SC

SIGMA = 0.012          # nominal quiet-air dev noise; swept in --sensitivity
EPS = 1e-6


# ═══════════════════════════════════════════════════════════ step 1: anchors ═

def z_of(channel: str, species_ppm: dict[str, float], sigma: float = SIGMA) -> float:
    arr = {k: np.array([v], float) for k, v in species_ppm.items()}
    return float(S.dev_from_species(channel, arr)[0] / sigma)


def triple(species_ppm: dict[str, float], sigma: float = SIGMA) -> tuple[float, float, float]:
    return tuple(z_of(ch, species_ppm, sigma) for ch in S.CHANNELS)


@dataclass
class Anchor:
    name: str
    what: str                 # the physical statement
    z2: float
    z7: float
    z135: float

    @property
    def r1(self) -> float:    # CO-ness
        return self.z7 / max(self.z2, EPS)

    @property
    def r2(self) -> float:    # smoke-ness
        return self.z135 / max(self.z2, EPS)


def build_anchors(sigma: float = SIGMA) -> dict[str, Anchor]:
    a = {}

    def add(key, what, sp):
        z2, z7, z135 = triple(sp, sigma)
        a[key] = Anchor(key, what, z2, z7, z135)

    lpg_lel = G.LPG.lel_ppm
    png_lel = G.PNG.lel_ppm

    add("gas_lpg_10lel", f"LPG at 10 % LEL = {0.10*lpg_lel:.0f} ppm (IEC 60079-29 first alarm)",
        {"lpg": 0.10 * lpg_lel})
    add("gas_lpg_25lel", f"LPG at 25 % LEL = {0.25*lpg_lel:.0f} ppm (evacuate)",
        {"lpg": 0.25 * lpg_lel})
    add("gas_png_10lel", f"PNG at 10 % LEL = {0.10*png_lel:.0f} ppm",
        {"png": 0.10 * png_lel})
    add("gas_png_25lel", f"PNG at 25 % LEL = {0.25*png_lel:.0f} ppm",
        {"png": 0.25 * png_lel})

    # The channel ratios are NOT constant with concentration. ln(1+x) is linear
    # at small x and logarithmic at large x, and the three channels have very
    # different C_base values, so their responses saturate at different rates.
    # For LPG the fuel ratio r2 runs from 1.61 at a whiff to 1.04 at 25 % LEL.
    # The contrast must subtract the LARGEST value or a small leak leaks into
    # the smoke channel — which is exactly the failure this catches.
    for f in (G.LPG, G.PNG):
        for frac in (0.005, 0.01, 0.05):
            add(f"{f.name.lower()}_trace_{frac}",
                f"{f.name} at {frac*100:.1f} % LEL — a whiff, used only to measure "
                f"the fuel signature where the response is still linear",
                {f.name.lower(): frac * f.lel_ppm})

    add("co_30", "CO 30 ppm — EN 50291 says MUST NOT alarm inside 120 min", {"co": 30.0})
    add("co_50", "CO 50 ppm — alarm between 60 and 90 min", {"co": 50.0})
    add("co_100", "CO 100 ppm — alarm between 10 and 40 min", {"co": 100.0})
    add("co_300", "CO 300 ppm — alarm within 3 min, no lower bound", {"co": 300.0})

    # Obscuration -> volatile concentration, using the pairing in scenarios.py.
    for key, (mg, obs) in SC.SMOKE_SOURCES.items():
        if mg <= 0:
            continue
        room = R.Room()
        c_vent = SC.voc_m3s(mg) / (room.lam(True) * room.volume_m3) * 1e6
        add(f"smoke_{key}", f"{key}: {obs:.1f} %/m obscuration, {c_vent:.0f} ppm volatiles "
                            f"(extractor running — the pessimistic case for detection)",
            {"smoke": c_vent})

    # EN 54-7 type-tests domestic photoelectric alarms to respond somewhere
    # between 2 and 8 %/m. Anchoring on the midpoint puts the knee above every
    # cooking source in the library and below every fire source, which is the
    # whole job. Interpolated linearly in emission rate against the pairing in
    # scenarios.py.
    room = R.Room()
    mid_mg = 900.0 * (4.5 / 6.0)
    mid_ppm = SC.voc_m3s(mid_mg) / (room.lam(True) * room.volume_m3) * 1e6
    add("smoke_en54_mid", f"4.5 %/m — inside EN 54-7's mandatory response "
                          f"band, {mid_ppm:.0f} ppm volatiles", {"smoke": mid_ppm})
    return a


# ══════════════════════════════════════════════════════ step 2-3: the model ══

@dataclass
class Params:
    """Everything the firmware needs. Every field is derived, none is chosen.

    The discriminants are LINEAR CONTRASTS rather than gates. A fuel leak has a
    fixed signature — z(MQ-7) ~ 1.9 z(MQ-2) and z(MQ-135) ~ 1.1 z(MQ-2) — so
    subtracting that signature off leaves zero for fuel and a large positive
    residue for anything else. Contrasts beat gates here because they degrade
    gracefully: a gate that sits between two classes weakens exactly when the
    event grows, since every channel's z climbs and the ratios converge.
    """
    sigma_nominal: float = SIGMA

    # the fuel signature, measured at the anchors
    r1_fuel: float = 0.0         # z_MQ7 / z_MQ2 for a pure fuel leak
    r2_fuel: float = 0.0         # z_MQ135 / z_MQ2 for a pure fuel leak
    gas_penalty: float = 0.75    # how hard a non-fuel residue suppresses gas

    x0_gas: float = 0.0
    k_gas: float = 1.0
    x0_co: float = 0.0
    k_co: float = 1.0
    x0_smoke: float = 0.0
    k_smoke: float = 1.0

    # CO dose. Carbon monoxide harm is cumulative — EN 50291's 50 ppm/90 min and
    # 100 ppm/40 min points are both dose limits, not level limits — so a level
    # threshold alone cannot satisfy the standard's shape. This integrator is
    # the second mechanism.
    co_dose_alarm: float = 0.0   # z-minutes equivalent to ~8 % COHb
    co_dose_tau_s: float = 0.0   # elimination half-life of COHb, as a decay
    co_ref_ppm: float = 0.0      # where the LEVEL knee landed, in ppm
    co_span: float = 0.0         # its 0.9 point, as a multiple of the knee

    # thermal / humidity
    dT60_fire: float = 0.0
    dT60_cook: float = 0.0
    dAH60_steam: float = 0.0
    steam_veto_z2_max: float = 0.0
    thermal_boost: float = 0.35

    # adaptation envelope
    adapt_max_raise: float = 1.60
    adapt_min_floor: float = 1.00

    # ---- the three discriminants ----------------------------------------
    def m_gas(self, z2, z7, z135):
        """Fuel magnitude: MQ-2's level, less whatever does not fit the fuel
        signature. Both residues are clipped at zero so a channel reading LOW
        relative to the signature cannot inflate the score."""
        res_smoke = np.maximum(z135 - self.r2_fuel * z2, 0.0)
        res_co = np.maximum(z7 - self.r1_fuel * z2, 0.0)
        return z2 - self.gas_penalty * (res_smoke + res_co)

    def m_co(self, z2, z7, z135):
        """CO magnitude: MQ-7 above what the fuel signature would explain. This
        is what stops an LPG leak — which lifts MQ-7 harder than it lifts MQ-2 —
        from being reported as carbon monoxide."""
        return z7 - self.r1_fuel * z2

    def m_smoke(self, z2, z7, z135):
        """Smoke magnitude: MQ-135 above the fuel signature. Combustion
        volatiles move MQ-135 about 3.6x as hard as they move MQ-2; fuel moves
        them together. That difference is the whole discriminator."""
        return z135 - self.r2_fuel * z2

    # ---- the three scores ------------------------------------------------
    def gas(self, z2, z7, z135):
        return _sig(self.m_gas(z2, z7, z135) - self.x0_gas, self.k_gas)

    def co(self, z2, z7, z135, dose=None):
        level = _sig(self.m_co(z2, z7, z135) - self.x0_co, self.k_co)
        if dose is None:
            return level
        # Dose and level are independent evidence of the same hazard; a noisy-OR
        # keeps either one able to alarm without them adding to more than one.
        d = _sig(dose - self.co_dose_alarm, self.co_dose_alarm * 0.25)
        return 1.0 - (1.0 - level) * (1.0 - d)

    def smoke(self, z2, z7, z135, dT60, dAH60):
        s = _sig(self.m_smoke(z2, z7, z135) - self.x0_smoke, self.k_smoke)
        s = s * (1.0 + self.thermal_boost *
                 np.clip(dT60 / max(self.dT60_fire, 1e-3), 0, 1))
        veto = (np.asarray(dAH60) > self.dAH60_steam) & \
               (np.asarray(z2) < self.steam_veto_z2_max)
        s = np.where(veto, s * 0.45, s)
        return np.clip(s, 0.0, 1.0)

    def co_dose_series(self, z2, z7, z135, dt: float):
        """Integrate the CO contrast with first-order elimination, in z-minutes."""
        m = np.maximum(self.m_co(z2, z7, z135), 0.0)
        out = np.empty_like(m)
        acc = 0.0
        k = math.exp(-dt / max(self.co_dose_tau_s, 1.0))
        for i, x in enumerate(m):
            acc = acc * k + x * dt / 60.0
            out[i] = acc
        return out


def _sig(x, k):
    k = max(float(k), 1e-6) if np.isscalar(k) else np.maximum(k, 1e-6)
    return 1.0 / (1.0 + np.exp(-np.clip(np.asarray(x, float) / k, -60.0, 60.0)))


def cohb_stewart(ppm: float, minutes: float, rmv_l_min: float = 10.0) -> float:
    """Stewart's equation for carboxyhaemoglobin saturation, % of haemoglobin.

        %COHb = 3.317e-5 * ppm^1.036 * RMV^0.41 * t^0.62

    This is what makes EN 50291's response envelope a coherent shape rather than
    four unrelated numbers: every point in it corresponds to roughly 8-10 % COHb,
    the level at which headache and impaired judgement begin.
    """
    return 3.317e-5 * (ppm ** 1.036) * (rmv_l_min ** 0.41) * (minutes ** 0.62) * 100.0


def fit_logistic(x50: float, x90: float) -> tuple[float, float]:
    """Solve for (x0, k) putting the logistic at 0.5 on x50 and 0.9 on x90."""
    return x50, max((x90 - x50) / math.log(9.0), 1e-3)


def derive(sigma: float = SIGMA, verbose: bool = True) -> tuple[Params, dict]:
    A = build_anchors(sigma)
    p = Params(sigma_nominal=sigma)

    # ---- the fuel signature ------------------------------------------------
    # Averaged over LPG and PNG at both alarm fractions. The two fuels sit close
    # enough in ratio space (r1 1.66-1.91, r2 0.99-1.10) that one signature
    # serves both, which is what lets a single build handle either kitchen.
    # Take the MAXIMUM ratio across the whole concentration range, not the mean.
    # The ratios fall as concentration rises (different channels saturate at
    # different rates), so a mean-fitted contrast under-subtracts at low
    # concentration and a small fuel leak bleeds into the smoke channel. The
    # maximum makes the fuel residue non-positive everywhere, at the cost of
    # subtracting a little extra from genuine smoke — which the smoke channel
    # can afford, because smoke's own separation is 3x.
    fuel_keys = [k for k in A if k.startswith(("gas_lpg", "gas_png", "lpg_trace", "png_trace"))]
    p.r1_fuel = round(float(np.max([A[k].r1 for k in fuel_keys])), 3)
    p.r2_fuel = round(float(np.max([A[k].r2 for k in fuel_keys])), 3)

    # ---- gas ---------------------------------------------------------------
    # 0.5 at LPG 10 % LEL, 0.9 at 25 % LEL. The LPG anchor is used even for a
    # PNG kitchen: PNG at 10 % LEL produces a LARGER response than LPG at 10 %
    # LEL, so an LPG-calibrated threshold trips a PNG leak at a smaller fraction
    # of its own LEL. Erring early is the correct direction.
    mg = lambda k: float(p.m_gas(A[k].z2, A[k].z7, A[k].z135))
    p.x0_gas, p.k_gas = fit_logistic(mg("gas_lpg_10lel"), mg("gas_lpg_25lel"))

    # ---- CO: level and dose, solved JOINTLY against the whole envelope -----
    #
    # These two cannot be anchored independently. EN 50291 has a lower bound at
    # every level as well as an upper one, and the lower bounds are what stop a
    # gas hob's ordinary output from setting the thing off. Anchoring the level
    # channel at 0.5 on 100 ppm — the obvious choice, and the one this file made
    # first — puts the alarm at 1 minute for a level the standard says must stay
    # silent for 10. The fix is not to nudge a constant; it is to recognise that
    # the level knee and the dose knee are one two-parameter problem with a
    # feasible region, and to search it.
    #
    # The shape of the solution is worth stating because it is not obvious: the
    # level channel ends up anchored well ABOVE 100 ppm, so that 300 ppm alarms
    # on level alone inside three minutes while 100 ppm has to wait for dose to
    # accumulate. Fast response and nuisance immunity come from different
    # mechanisms, which is exactly why one threshold could never deliver both.
    tau = 4.0 * 3600.0                      # COHb elimination, ~4-5 h
    p.co_dose_tau_s = tau
    mc = lambda k: float(p.m_co(A[k].z2, A[k].z7, A[k].z135))

    def solve_dose(x0, k_) -> float:
        """Dose knee that puts the COMBINED score across 0.5 at 75 min of
        50 ppm — the middle of the standard's 60-90 minute window."""
        level = float(_sig(mc("co_50") - x0, k_))
        d_raw = mc("co_50") * (tau / 60.0) * (1.0 - math.exp(-75 * 60.0 / tau))
        lo_, hi_ = 1.0, 1e7
        for _ in range(200):
            mid = 0.5 * (lo_ + hi_)
            combined = 1.0 - (1.0 - level) * (1.0 - _sig(d_raw - mid, mid * 0.25))
            if combined > 0.5:
                lo_ = mid
            else:
                hi_ = mid
        return hi_

    def envelope_score(x0, k_, alarm):
        """How well centred each corner is inside its window. -inf if outside."""
        limits = {"co_30": (30, 120, None), "co_50": (50, 60, 90),
                  "co_100": (100, 10, 40), "co_300": (300, None, 3)}
        total = 0.0
        for key, (ppm, lo_, hi_) in limits.items():
            m = mc(key)
            level = float(_sig(m - x0, k_))
            t_hit = None
            for step in range(1, 3 * 60 + 1):
                d = m * (tau / 60.0) * (1.0 - math.exp(-step * 60.0 / tau))
                if 1.0 - (1.0 - level) * (1.0 - _sig(d - alarm, alarm * 0.25)) >= 0.5:
                    t_hit = step
                    break
            if lo_ is not None and (t_hit is not None and t_hit < lo_):
                return float("-inf")
            if hi_ is not None and (t_hit is None or t_hit > hi_):
                return float("-inf")
            # margin, in log-time, to whichever bound is nearer
            if t_hit is not None:
                if lo_ and hi_:
                    total += min(math.log(t_hit / lo_), math.log(hi_ / t_hit))
                elif hi_:
                    total += math.log(hi_ / t_hit)
                else:
                    total += math.log(t_hit / lo_)
            else:
                total += math.log(180.0 / lo_)
        return total

    best = None
    for ppm_ref in range(90, 601, 10):
        z2r, z7r, z135r = triple({"co": float(ppm_ref)}, sigma)
        x0 = float(p.m_co(z2r, z7r, z135r))
        for span in (2.0, 2.5, 3.0, 4.0):     # 0.9 point at span x ppm_ref
            z2s, z7s, z135s = triple({"co": float(ppm_ref) * span}, sigma)
            k_ = max((float(p.m_co(z2s, z7s, z135s)) - x0) / math.log(9.0), 1e-3)
            alarm = solve_dose(x0, k_)
            sc_ = envelope_score(x0, k_, alarm)
            if best is None or sc_ > best[0]:
                best = (sc_, x0, k_, alarm, ppm_ref, span)
    if best is None or best[0] == float("-inf"):
        raise SystemExit("no CO operating point satisfies EN 50291's envelope")
    _, p.x0_co, p.k_co, p.co_dose_alarm, p.co_ref_ppm, p.co_span = best
    p.co_dose_alarm = round(p.co_dose_alarm, 1)

    # ---- smoke -------------------------------------------------------------
    # EN 54-7 type-tests domestic photoelectric alarms between 2 and 8 %/m.
    # Cooking routinely reaches 2.5 %/m, which is why kitchen alarms set at the
    # bottom of that band end up taped over. Anchor 0.5 at burning food (6 %/m)
    # and 0.9 at a flaming fire (14 %/m), leaving cooking smoke below the knee.
    ms = lambda k: float(p.m_smoke(A[k].z2, A[k].z7, A[k].z135))
    p.x0_smoke, p.k_smoke = fit_logistic(ms("smoke_en54_mid"),
                                         ms("smoke_flaming_fire"))

    # ---- thermal and humidity ---------------------------------------------
    room = R.Room()
    n = int(300 / SC.DT)
    lam_off = lambda t: room.lam(False)
    dT_fire = R.thermal(room, SC.DT, n, lambda t: G.HRR_KW["pan_oil_developed"], lam_off)
    dT_cook = R.thermal(room, SC.DT, n, lambda t: G.HRR_KW["burner_high"], lam_off)
    s60 = int(60 / SC.DT)
    p.dT60_fire = round(float(np.max(dT_fire[s60:] - dT_fire[:-s60])), 3)
    p.dT60_cook = round(float(np.max(dT_cook[s60:] - dT_cook[:-s60])), 3)

    ah_steam, _ = R.humidity(room, SC.DT, n,
                             lambda t: G.STEAM_G_MIN["rice_uncovered"] / 60.0,
                             lam_off, np.zeros(n))
    dAH_steam = float(np.max(ah_steam[s60:] - ah_steam[:-s60]))
    p.dAH60_steam = round(0.60 * dAH_steam, 3)
    p.steam_veto_z2_max = round(0.5 * A["gas_lpg_10lel"].z2, 1)

    if verbose:
        _report(A, p, sigma)
        _en50291(p, A)
    return p, A


def en50291_times(p: "Params", A: dict) -> dict:
    """Minutes until the combined CO score crosses 0.5 at each test level."""
    tau = p.co_dose_tau_s
    out = {}
    for key, ppm in (("co_30", 30), ("co_50", 50), ("co_100", 100), ("co_300", 300)):
        a = A[key]
        m = float(p.m_co(a.z2, a.z7, a.z135))
        level = float(_sig(m - p.x0_co, p.k_co))
        t = None
        for step in range(1, 3 * 60 + 1):          # up to 3 h, 1-minute steps
            d = m * (tau / 60.0) * (1.0 - math.exp(-step * 60.0 / tau))
            if 1.0 - (1.0 - level) * (1.0 - _sig(d - p.co_dose_alarm,
                                                 p.co_dose_alarm * 0.25)) >= 0.5:
                t = step
                break
        out[ppm] = t
    return out


def _en50291(p: "Params", A: dict):
    """EN 50291-1's response envelope, as a pass/fail table.

    The standard has BOTH bounds for a reason. The upper bound is the safety
    requirement. The lower bound is the nuisance requirement, and it is the one
    that decides whether a device stays on the wall: an alarm that goes off at
    35 ppm because someone is frying gets unplugged, and an unplugged alarm
    protects nobody.
    """
    limits = {30: (120, None), 50: (60, 90), 100: (10, 40), 300: (None, 3)}
    t = en50291_times(p, A)
    print(f"\n{'='*78}\nEN 50291-1 RESPONSE ENVELOPE\n{'='*78}")
    print(f"  {'ppm':>5} {'alarms at':>12} {'must be after':>15} {'must be before':>16}  verdict")
    ok_all = True
    for ppm, (lo, hi) in limits.items():
        tt = t[ppm]
        shown = f"{tt} min" if tt else "never"
        good = True
        if lo is not None and (tt is not None and tt < lo):
            good = False
        if hi is not None and (tt is None or tt > hi):
            good = False
        ok_all &= good
        print(f"  {ppm:>5} {shown:>12} {str(lo)+' min' if lo else '-':>15} "
              f"{str(hi)+' min' if hi else '-':>16}  {'ok' if good else 'FAIL'}")
    print(f"  -> {'compliant' if ok_all else 'NOT COMPLIANT'} "
          f"(this device is not certified to the standard; the envelope is used\n"
          f"     here because it is the best published description of what a\n"
          f"     domestic CO alarm should do, not as a claim of conformity)")


# ═══════════════════════════════════════════════════════════ step 4: verify ══

def evaluate(p: Params, fuel: G.Fuel = G.LPG, sigma: float = SIGMA,
             trials: int = 8) -> list[dict]:
    """Every scenario is run several times with different noise draws.

    A single draw is not evidence. Sensor noise and drift move the contrasts by
    a few z either way, which is enough to flip a marginal scenario across the
    knee — so a benign case is judged on its WORST draw and a hazardous case on
    its worst draw too. Both directions are the pessimistic one.
    """
    room = R.Room()
    noise = S.SensorNoise(dev_sigma=sigma)
    rows = []
    for sc in SC.library(fuel):
        trial_rows = [_one(p, sc, room, noise, i) for i in range(trials)]
        benign = sc.severity == 0
        # benign: the draw that came closest to alarming. hazard: the draw that
        # came closest to missing.
        key = (lambda r: r["peak_any"]) if benign else (lambda r: -r["peak_any"])
        rows.append(max(trial_rows, key=key))
    return rows


def _one(p: Params, sc, room, noise, trial: int) -> dict:
    if True:
        d = sc.build(room, noise, trial=trial)
        z = d["dev"] / np.maximum(d["dev_scale"], 1e-9)
        s60 = int(60 / SC.DT)
        dT60 = np.concatenate([np.zeros(s60), d["dT"][s60:] - d["dT"][:-s60]])
        dAH60 = np.concatenate([np.zeros(s60), d["ah"][s60:] - d["ah"][:-s60]])

        dose = p.co_dose_series(z[0], z[1], z[2], SC.DT)
        # Raw Model A output, then the same temporal conditioning the fusion
        # layer applies before any of this can reach the state machine.
        g = S.condition(p.gas(z[0], z[1], z[2]), SC.DT)
        c = S.condition(p.co(z[0], z[1], z[2], dose), SC.DT)
        sm = S.condition(p.smoke(z[0], z[1], z[2], dT60, dAH60), SC.DT)
        worst = np.maximum.reduce([g, c, sm])

        onset = int(sc.onset_s / SC.DT)
        det = np.argmax(worst[onset:] > 0.5) if np.any(worst[onset:] > 0.5) else -1
        return dict(
            name=d["name"], sev=d["severity"], fuel=d["fuel"],
            deadline_s=sc.deadline_s,
            peak_gas=float(g.max()), peak_co=float(c.max()), peak_smoke=float(sm.max()),
            peak_any=float(worst.max()),
            ttd_s=(det * SC.DT if det >= 0 else None),
            max_frac_lel=float(d["frac_lel"].max()),
            max_co_ppm=float(d["co_ppm"].max()),
            max_obsc=float(d["obscuration"].max()),
            max_dT=float(d["dT"].max()),
        )


def check(rows: list[dict]) -> tuple[bool, list[str]]:
    """The constraint set. A derivation that violates any of it is not emitted.

    The bars differ by severity because the architectural contract differs.
    Model A is not the alarm; it is one witness feeding a fusion layer that also
    hears the camera, the thermometer and the corroboration bonuses.

      severity 0    must not push the system toward WARNING on its own.
                    Bar: stay under 0.45.

      severity 3    must supply substantial evidence, but is entitled to need
                    corroboration to reach an alarm. Two cases sit here for
                    honest reasons rather than fixable ones.

                    Burning food at 6 %/m obscuration is only 1.3x above the
                    4.5 %/m knee, and nothing separates it cleanly from smoky
                    cooking at 2.5 %/m on a single broadband MOX channel. That
                    is a property of the sensor, and the right answer is to
                    require the camera to agree rather than to drag the knee
                    down into the cooking band.

                    Carbon monoxide near 50 ppm is marginal BY REQUIREMENT: the
                    standard says it must take an hour. Anything that made it
                    reliably cross a high bar quickly would violate the lower
                    bound the standard sets to stop nuisance alarms.

                    Bar: reach 0.40.

      severity 4-5  must be able to alarm ALONE, because the camera may be
                    blind, blocked, or looking the wrong way, and those are
                    exactly the cases where that cannot be allowed to matter.
                    Bar: reach 0.60 inside the hazard's own deadline.
    """
    problems = []
    benign = [r for r in rows if r["sev"] == 0]
    haz3 = [r for r in rows if r["sev"] == 3]
    severe = [r for r in rows if r["sev"] >= 4]

    for r in benign:
        if r["peak_any"] >= 0.45:
            problems.append(f"FALSE ALARM: benign '{r['name']}' peaked at {r['peak_any']:.2f}")
    for r in haz3:
        if r["peak_any"] < 0.40:
            problems.append(f"WEAK: severity-3 '{r['name']}' peaked only {r['peak_any']:.2f}")
    for r in severe:
        if r["peak_any"] < 0.60:
            problems.append(f"MISS: severity-{r['sev']} '{r['name']}' peaked only "
                            f"{r['peak_any']:.2f}")
        elif r["ttd_s"] is None:
            problems.append(f"MISS: severe '{r['name']}' never crossed 0.5")
        elif r["ttd_s"] > r["deadline_s"]:
            problems.append(f"SLOW: severe '{r['name']}' took {r['ttd_s']:.0f} s, "
                            f"deadline {r['deadline_s']:.0f} s")

    hazard = haz3 + severe
    if benign and hazard:
        margin = min(r["peak_any"] for r in hazard) - max(r["peak_any"] for r in benign)
        if margin < 0.15:
            problems.append(f"THIN MARGIN: only {margin:.2f} between worst benign "
                            f"and weakest hazard")
    return (len(problems) == 0), problems


# ══════════════════════════════════════════════════════════════ reporting ═══

def _report(A: dict, p: Params, sigma: float):
    print(f"\n{'='*78}\nANCHORS  (quiet-air noise width sigma = {sigma:.4f} dev units)\n{'='*78}")
    print(f"{'anchor':<22}{'z_MQ2':>9}{'z_MQ7':>9}{'z_MQ135':>10}{'r1=7/2':>9}{'r2=135/2':>10}")
    for k, a in A.items():
        print(f"{k:<22}{a.z2:>9.1f}{a.z7:>9.1f}{a.z135:>10.1f}{a.r1:>9.2f}{a.r2:>10.2f}")

    print(f"\n{'='*78}\nWHAT EACH ANCHOR MEANS\n{'='*78}")
    for k, a in A.items():
        print(f"  {k:<22} {a.what}")

    print(f"\n{'='*78}\nDERIVED PARAMETERS\n{'='*78}")
    print(f"  fuel signature: r1 = {p.r1_fuel:.3f}, r2 = {p.r2_fuel:.3f}")
    print(f"  gas   logistic: 0.5 at contrast {p.x0_gas:7.1f}, width {p.k_gas:6.2f}")
    print(f"  CO    logistic: 0.5 at contrast {p.x0_co:7.1f}, width {p.k_co:6.2f}")
    print(f"  smoke logistic: 0.5 at contrast {p.x0_smoke:7.1f}, width {p.k_smoke:6.2f}")
    print(f"  CO dose alarm:  {p.co_dose_alarm:.1f} z-minutes "
          f"(= 50 ppm held 90 min, {cohb_stewart(50, 90):.1f} % COHb)")
    print(f"\n  Contrast values at each anchor (this is the separation):")
    print(f"    {'anchor':<22}{'m_gas':>9}{'m_co':>9}{'m_smoke':>9}")
    for k, a_ in A.items():
        print(f"    {k:<22}{float(p.m_gas(a_.z2,a_.z7,a_.z135)):>9.1f}"
              f"{float(p.m_co(a_.z2,a_.z7,a_.z135)):>9.1f}"
              f"{float(p.m_smoke(a_.z2,a_.z7,a_.z135)):>9.1f}")
    print(f"  thermal: fire makes {p.dT60_fire:.2f} K/60 s, a burner makes {p.dT60_cook:.2f} K/60 s")
    print(f"  steam veto: dAH60 > {p.dAH60_steam:.2f} g/m3 with z_MQ2 < {p.steam_veto_z2_max:.1f}")

    print(f"\n  Sanity — what the derived gas threshold corresponds to physically:")
    for fuel in (G.LPG, G.PNG):
        for frac in (0.05, 0.10, 0.25):
            ppm = frac * fuel.lel_ppm
            sp = {fuel.name.lower(): ppm}
            z2, z7, z135 = triple(sp, sigma)
            print(f"    {fuel.name} {frac*100:4.0f} % LEL ({ppm:6.0f} ppm) -> gas score "
                  f"{float(p.gas(z2, z7, z135)):.3f}")
    for ppm in (30, 50, 100, 300):
        z2, z7, z135 = triple({"co": float(ppm)}, sigma)
        mins = 90 if ppm <= 50 else 40 if ppm <= 100 else 3
        dose = p.m_co(z2, z7, z135) * (p.co_dose_tau_s/60.0) * \
               (1.0 - math.exp(-mins*60.0/p.co_dose_tau_s))
        print(f"    CO  {ppm:4d} ppm  level {float(p.co(z2,z7,z135)):.3f}   "
              f"after {mins:3d} min with dose {float(p.co(z2,z7,z135,dose)):.3f}   "
              f"({cohb_stewart(ppm, mins):.1f} % COHb)")


def report_scenarios(rows: list[dict]):
    print(f"\n{'='*98}\nSCENARIO SWEEP\n{'='*98}")
    print(f"{'scenario':<26}{'sev':>4}{'gas':>7}{'co':>7}{'smoke':>7}{'peak':>7}"
          f"{'t_det':>8}{'%LEL':>8}{'CO ppm':>9}{'%/m':>7}{'dT K':>7}")
    for r in sorted(rows, key=lambda x: (x["sev"], x["name"])):
        ttd = f"{r['ttd_s']:.0f}s" if r["ttd_s"] is not None else "-"
        print(f"{r['name']:<26}{r['sev']:>4}{r['peak_gas']:>7.2f}{r['peak_co']:>7.2f}"
              f"{r['peak_smoke']:>7.2f}{r['peak_any']:>7.2f}{ttd:>8}"
              f"{r['max_frac_lel']*100:>8.1f}{r['max_co_ppm']:>9.0f}"
              f"{r['max_obsc']:>7.1f}{r['max_dT']:>7.2f}")


# ═════════════════════════════════════════════════════════════════ emit ═════

HEADER_TMPL = '''/* ============================================================================
   physics_thresholds.h  —  GENERATED FILE, DO NOT EDIT BY HAND.

   Produced by  training/physics/derive_thresholds.py
   Regenerate with:
       cd training && python -m physics.derive_thresholds --emit \\
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

   Room model: {volume:.0f} m3, {ach_closed:.1f} ACH closed / {ach_open:.1f} ACH extracting,
   sensor at {sensor_h:.1f} m of a {ceiling:.1f} m ceiling, ambient {t_amb:.0f} C / {rh_amb:.0f} %RH.
   Quiet-air noise width assumed: sigma = {sigma:.4f} dev units.

   Verified against {n_scen} simulated scenarios per fuel, {n_trial} noise draws each,
   judged on the worst draw in both directions. Generated {stamp}.
   ========================================================================== */
#pragma once

// ─────────────────────────────────────────────────────── fuel selection ────
// 0 = AUTO   use the LPG-calibrated thresholds. PNG at 10 % LEL produces a
//            LARGER sensor response than LPG at 10 % LEL, so these thresholds
//            trip a PNG leak at about {png_equiv:.0f} % of ITS lower explosive limit —
//            early, which is the correct direction to err. Leave this alone
//            unless you know which gas your kitchen burns.
// 1 = LPG    cylinder. Denser than air (rel. density {rd_lpg:.2f}): it pools at floor
//            level, so mount the MQ sensors LOW — 30 cm above the floor.
// 2 = PNG    piped methane. Lighter than air (rel. density {rd_png:.2f}): it collects at
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
//        pure LPG at 10 % LEL   r1 = {r1_lpg:5.2f}   r2 = {r2_lpg:5.2f}
//        pure PNG at 10 % LEL   r1 = {r1_png:5.2f}   r2 = {r2_png:5.2f}
//        burning food           r1 = {r1_smoke:5.2f}   r2 = {r2_smoke:5.2f}
//        pure CO at 100 ppm     r1 = {r1_co:5.2f}   r2 = {r2_co:5.2f}
//
//   m_gas   = z2 - {gas_penalty:.2f} * ( max(z135 - r2*z2, 0) + max(z7 - r1*z2, 0) )
//   m_co    = z7   - r1 * z2
//   m_smoke = z135 - r2 * z2
#define PZ_R1_FUEL        {r1_fuel:.3f}f
#define PZ_R2_FUEL        {r2_fuel:.3f}f
#define PZ_GAS_PENALTY    {gas_penalty:.2f}f

// ──────────────────────────────────────────────── logistic operating points ─
// x0 is the contrast at which the score reads 0.5; k is its width.
//
// gas:   0.5 at LPG 10 % LEL ({lpg10:.0f} ppm, IEC 60079-29's first alarm),
//        0.9 at 25 % LEL ({lpg25:.0f} ppm, the evacuate point).
#define PZ_X0_GAS         {x0_gas:.2f}f
#define PZ_K_GAS          {k_gas:.2f}f

// CO:    0.5 at 100 ppm and 0.9 at 300 ppm — the two mandatory points in
//        EN 50291's response envelope. 30 ppm lands at {audit_co30:.2f}, which
//        discharges the standard's must-NOT-alarm requirement by construction.
#define PZ_X0_CO          {x0_co:.2f}f
#define PZ_K_CO           {k_co:.2f}f

// smoke: 0.5 at 4.5 %/m obscuration — inside EN 54-7's 2-8 %/m mandatory band
//        and 1.8x above the smokiest routine cooking in the scenario library.
//        0.9 at 14 %/m, a developed flaming fire.
#define PZ_X0_SMOKE       {x0_smoke:.2f}f
#define PZ_K_SMOKE        {k_smoke:.2f}f

// ───────────────────────────────────────────────────────── carbon monoxide ─
// EN 50291's envelope is a set of DOSE limits wearing level clothing. Pushed
// through Stewart's equation every point in it lands near 8-10 % COHb:
//
//        50 ppm for 90 min  ->  {cohb50:.1f} % COHb
//       100 ppm for 40 min  ->  {cohb100:.1f} % COHb
//       300 ppm for  3 min  ->  {cohb300:.1f} % COHb
//
// A level threshold cannot express that shape, so there is a second integrating
// channel. Its decay is the elimination half-life of carboxyhaemoglobin, which
// is why the integrator forgets at the rate a person recovers.
#define PZ_CO_DOSE_ALARM  {co_dose_alarm:.1f}f    // z-minutes
#define PZ_CO_DOSE_TAU_S  {co_dose_tau_s:.1f}f    // {co_tau_h:.1f} h

// ───────────────────────────────────────────────────── thermal / humidity ──
// A developed pan fire ({hrr_fire:.0f} kW) raises room air {dT60_fire:.2f} K per 60 s in this
// room. A gas burner at full ({hrr_cook:.1f} kW) manages {dT60_cook:.2f} K. That order of
// magnitude is the thermal channel's entire discriminating power, which is why
// it corroborates rather than testifying on its own.
#define PZ_DT60_FIRE      {dT60_fire:.3f}f
#define PZ_DT60_COOK      {dT60_cook:.3f}f
#define PZ_THERMAL_BOOST  {thermal_boost:.2f}f

// An uncovered pot moves ABSOLUTE humidity hard and temperature barely at all.
// A fire does the reverse: it raises temperature, which drops RELATIVE humidity
// while absolute humidity stays nearly flat. Vetoing on absolute humidity is
// what stops a pot of rice from reading as combustion; vetoing on relative
// humidity would do the opposite of what you want.
#define PZ_DAH60_STEAM    {dAH60_steam:.3f}f
#define PZ_STEAM_Z2_MAX   {steam_veto_z2_max:.1f}f

// ─────────────────────────────────────────────── adaptation safety envelope ─
// adapt.h may attenuate evidence in the nuisance band. It may not move any
// constant in this file, and it may not reach evidence above its hard-keep
// fence. These are published here so the bound is visible from one place.
#define PZ_ADAPT_MAX_RAISE  {adapt_max_raise:.2f}f
#define PZ_ADAPT_MIN_FLOOR  {adapt_min_floor:.2f}f
#define PZ_FLOOR_X0_GAS     {x0_gas:.2f}f
#define PZ_FLOOR_X0_CO      {x0_co:.2f}f
#define PZ_FLOOR_X0_SMOKE   {x0_smoke:.2f}f

// ──────────────────────────────────────────────── what these numbers mean ──
// Printed by the firmware's 'p' command, so the operating point is auditable on
// the bench and not only in this file.
#define PZ_AUDIT_GAS_10LEL   {audit_gas10:.3f}f
#define PZ_AUDIT_GAS_25LEL   {audit_gas25:.3f}f
#define PZ_AUDIT_CO_30       {audit_co30:.3f}f
#define PZ_AUDIT_CO_100      {audit_co100:.3f}f
#define PZ_AUDIT_CO_300      {audit_co300:.3f}f
#define PZ_SIGMA_NOMINAL     {sigma:.4f}f
'''


def emit(path: str, p: Params, A: dict, sigma: float):
    import datetime
    room = R.Room()

    # What fraction of PNG's LEL trips the LPG-calibrated threshold. Bisection
    # rather than algebra because m_gas has clipped terms in it.
    lo, hi = 1.0, G.PNG.lel_ppm
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        z2, z7, z135 = triple({"png": mid}, sigma)
        if float(p.m_gas(z2, z7, z135)) < p.x0_gas:
            lo = mid
        else:
            hi = mid
    png_equiv = 100.0 * lo / G.PNG.lel_ppm

    def audit_gas(key):
        a = A[key]
        return float(p.gas(a.z2, a.z7, a.z135))

    def audit_co(key):
        a = A[key]
        return float(p.co(a.z2, a.z7, a.z135))

    txt = HEADER_TMPL.format(
        volume=room.volume_m3, ach_closed=room.ach_closed, ach_open=room.ach_extractor,
        sensor_h=room.sensor_height_m, ceiling=room.ceiling_m,
        t_amb=room.t_ambient_c, rh_amb=room.rh_ambient_pct, sigma=sigma,
        stamp=datetime.date.today().isoformat(),
        n_scen=len(SC.library(G.LPG)), n_trial=8,
        png_equiv=png_equiv, rd_lpg=G.LPG.rel_density_air, rd_png=G.PNG.rel_density_air,
        lpg10=0.10 * G.LPG.lel_ppm, lpg25=0.25 * G.LPG.lel_ppm,
        r1_lpg=A["gas_lpg_10lel"].r1, r2_lpg=A["gas_lpg_10lel"].r2,
        r1_png=A["gas_png_10lel"].r1, r2_png=A["gas_png_10lel"].r2,
        r1_smoke=A["smoke_burning_food"].r1, r2_smoke=A["smoke_burning_food"].r2,
        r1_co=A["co_100"].r1, r2_co=A["co_100"].r2,
        cohb50=cohb_stewart(50, 90), cohb100=cohb_stewart(100, 40),
        cohb300=cohb_stewart(300, 3), co_tau_h=p.co_dose_tau_s / 3600.0,
        hrr_fire=G.HRR_KW["pan_oil_developed"], hrr_cook=G.HRR_KW["burner_high"],
        audit_gas10=audit_gas("gas_lpg_10lel"), audit_gas25=audit_gas("gas_lpg_25lel"),
        audit_co30=audit_co("co_30"), audit_co100=audit_co("co_100"),
        audit_co300=audit_co("co_300"),
        **{k: v for k, v in asdict(p).items() if k != "sigma_nominal"},
    )
    with open(path, "w") as f:
        f.write(txt)
    print(f"\nwrote {path}  ({len(txt)} bytes)")


# ═════════════════════════════════════════════════════════════════ main ═════

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--emit", metavar="PATH")
    ap.add_argument("--sigma", type=float, default=SIGMA)
    ap.add_argument("--sensitivity", action="store_true",
                    help="sweep the assumed noise width and check the verdict holds")
    a = ap.parse_args()

    p, A = derive(a.sigma, verbose=a.report or a.emit is not None)

    ok_all = True
    for fuel in (G.LPG, G.PNG):
        rows = evaluate(p, fuel, a.sigma)
        if a.report:
            print(f"\n\n### FUEL = {fuel.name}")
            report_scenarios(rows)
        ok, problems = check(rows)
        ok_all &= ok
        print(f"\n[{fuel.name}] {'PASS' if ok else 'FAIL'}")
        for pr in problems:
            print("   ", pr)

    if a.sensitivity:
        print(f"\n{'='*78}\nSENSITIVITY TO THE ASSUMED NOISE WIDTH\n{'='*78}")
        print("The whole derivation divides by sigma, so if the verdict flips when")
        print("sigma moves within its plausible range, the thresholds are not robust.")
        print("Drift is modelled as a multiple of sigma, so a correct derivation")
        print("should be very nearly sigma-invariant: everything downstream works")
        print("in z-scores. Residual movement comes from the humidity term, which")
        print("is an absolute deviation and does NOT scale with the noise floor.\n")
        for s in (0.006, 0.009, 0.012, 0.018, 0.025, 0.035):
            pp, _ = derive(s, verbose=False)
            rr = evaluate(pp, G.LPG, s, trials=4)
            ok, probs = check(rr)
            worst_benign = max((r["peak_any"] for r in rr if r["sev"] == 0), default=0)
            best_hazard = min((r["peak_any"] for r in rr if r["sev"] >= 3), default=1)
            print(f"  sigma={s:.3f}  x0_gas={pp.x0_gas:7.1f}  x0_co={pp.x0_co:6.1f}  "
                  f"x0_smoke={pp.x0_smoke:7.1f}  benign<={worst_benign:.2f} "
                  f"hazard>={best_hazard:.2f}  ->  "
                  f"{'PASS' if ok else 'FAIL(' + str(len(probs)) + ')'}")
            for pr in probs:
                print(f"        {pr}")

    if a.emit:
        if not ok_all:
            print("\nREFUSING to emit: the constraint set is not satisfied.")
            sys.exit(1)
        emit(a.emit, p, A, a.sigma)


if __name__ == "__main__":
    main()
