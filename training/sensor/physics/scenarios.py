"""
scenarios.py — the situations the thresholds have to be right about.

Each scenario is built from physical source terms, not from drawn curves. A
scenario says "an unlit 2.8 kW burner is open from t=60 s with the extractor
off"; the room model works out what that does to the air, and the sensor model
works out what the firmware sees. Nothing here is fitted to a desired outcome.

Severity is assigned from the published limits in gas_data, not from intuition:

  0  benign, must never alarm
  1  benign but signal-producing, may reach WARNING
  2  worth noticing
  3  hazardous, must alarm
  4  dangerous, must alarm fast
  5  life-threatening, must alarm within seconds
"""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import Callable
import zlib
import numpy as np

from . import gas_data as G
from . import room as R
from . import sensors as S

DT = 0.5                      # firmware tick, seconds
VOC_MW = 50.0                 # g/mol, nominal pyrolysis volatile


def _step(t0: float, t1: float, value: float) -> Callable[[float], float]:
    return lambda t: value if t0 <= t < t1 else 0.0


def _ramp(t0: float, t1: float, v0: float, v1: float) -> Callable[[float], float]:
    def f(t):
        if t < t0:
            return 0.0
        if t >= t1:
            return v1
        return v0 + (v1 - v0) * (t - t0) / max(t1 - t0, 1e-6)
    return f


def _sum(*fns) -> Callable[[float], float]:
    return lambda t: sum(f(t) for f in fns)


def voc_m3s(mg_min: float) -> float:
    """Pyrolysis volatiles, mg/min -> m^3/s at 25 C."""
    return (mg_min / 1000.0 / 60.0) / VOC_MW * G.MOLAR_VOL_25C / 1000.0


# Volatile emission and the obscuration that goes with it. Obscuration is the
# quantity smoke alarms are type-tested against (EN 54-7), so pinning the two
# together lets a %/m figure be turned into an MQ threshold.
SMOKE_SOURCES = {
    #                   mg/min VOC, obscuration %/m at steady state in 25 m^3
    "none":            (0.0,     0.0),
    "cooking_normal":  (15.0,    0.25),
    "frying":          (60.0,    0.9),
    "cooking_smoky":   (250.0,   2.5),
    "burning_food":    (900.0,   6.0),
    "oil_pyrolysis":   (2200.0,  10.0),
    "flaming_fire":    (4000.0,  14.0),
}


@dataclass
class Scenario:
    name: str
    label: int
    severity: int
    duration_s: float = 900.0
    onset_s: float = 120.0
    # How long this hazard is ALLOWED to take. Fire and explosive accumulation
    # are seconds-to-minutes problems; CO is a dose problem with its own, much
    # longer, published envelope.
    deadline_s: float = 120.0
    extractor: bool = True
    fuel: G.Fuel = field(default=G.LPG)

    gas_src: Callable[[float], float] = field(default=lambda t: 0.0)   # m^3/s
    co_mg_min: Callable[[float], float] = field(default=lambda t: 0.0)
    smoke_key: Callable[[float], str] = field(default=lambda t: "none")
    steam_g_min: Callable[[float], float] = field(default=lambda t: 0.0)
    hrr_kw: Callable[[float], float] = field(default=lambda t: 0.0)

    # Vision ground truth, 0..1
    open_flame: Callable[[float], float] = field(default=lambda t: 0.0)
    burner_flame: Callable[[float], float] = field(default=lambda t: 0.0)
    person: Callable[[float], float] = field(default=lambda t: 1.0)
    cam_conf: Callable[[float], float] = field(default=lambda t: 0.9)

    def seed(self, trial: int = 0) -> int:
        """Deterministic per-scenario seed. Python's str hash is salted per
        process, so using it here made every run of the derivation produce
        different thresholds — which is exactly the kind of irreproducibility
        that makes a safety argument worthless."""
        return (zlib.crc32(self.name.encode()) + 7919 * trial) & 0x7FFFFFFF

    def build(self, room: R.Room, noise: S.SensorNoise,
              rng: np.random.Generator | None = None, trial: int = 0) -> dict:
        n = int(self.duration_s / DT)
        t = np.arange(n) * DT
        lam = lambda _t: room.lam(self.extractor)

        # ---- species concentrations, ppm -----------------------------------
        strat = room.stratification_factor(self.fuel.rel_density_air)
        gas_vf = R.integrate(room, DT, n, self.gas_src, lam)
        gas_ppm = gas_vf * 1e6 * strat

        co_vf = R.integrate(room, DT, n,
                            lambda tt: R.co_flow_m3s(self.co_mg_min(tt)), lam)
        co_ppm = co_vf * 1e6

        smoke_vf = R.integrate(
            room, DT, n,
            lambda tt: voc_m3s(SMOKE_SOURCES[self.smoke_key(tt)][0]), lam,
            lam_dep=2.5e-4)          # particulates do deposit; gases do not
        smoke_ppm = smoke_vf * 1e6

        obsc_ss = np.array([SMOKE_SOURCES[self.smoke_key(tt)][1] for tt in t])
        # obscuration follows the same first-order lag as the volatiles
        obsc = obsc_ss * np.clip(smoke_ppm / max(smoke_ppm.max(), 1e-9), 0, 1) \
            if smoke_ppm.max() > 0 else np.zeros(n)

        # ---- thermal and humidity ------------------------------------------
        dT = R.thermal(room, DT, n, self.hrr_kw, lam)
        ah, rh = R.humidity(room, DT, n,
                            lambda tt: self.steam_g_min(tt) / 60.0, lam, dT)
        t_c = room.t_ambient_c + dT

        species = {"lpg": gas_ppm if self.fuel is G.LPG else np.zeros(n),
                   "png": gas_ppm if self.fuel is G.PNG else np.zeros(n),
                   "co": co_ppm,
                   "smoke": smoke_ppm}

        # ---- what the firmware sees ----------------------------------------
        rng = rng or np.random.default_rng(self.seed(trial))
        common = S.drift_walk(n, DT, noise.drift_common_per_hour, rng)
        # devScale may only be estimated from air that is still quiet, so the
        # window has to close before this scenario's source turns on.
        quiet_n = int(max(60.0, self.onset_s - 10.0) / DT)
        dev_obs, dev_scale = [], []
        for ch in S.CHANNELS:
            true = S.dev_from_species(ch, species, rh, t_c)
            o, sc = S.observed_dev(true, DT, noise, rng, common, quiet_n)
            dev_obs.append(o)
            dev_scale.append(sc)

        return dict(
            name=self.name, label=self.label, severity=self.severity,
            t=t, onset_s=self.onset_s,
            dev=np.array(dev_obs), dev_scale=np.array(dev_scale),
            gas_ppm=gas_ppm, co_ppm=co_ppm, smoke_ppm=smoke_ppm,
            obscuration=obsc, t_c=t_c, rh=rh, ah=ah, dT=dT,
            frac_lel=gas_ppm / self.fuel.lel_ppm,
            open_flame=np.array([self.open_flame(x) for x in t]),
            burner_flame=np.array([self.burner_flame(x) for x in t]),
            person=np.array([self.person(x) for x in t]),
            cam_conf=np.array([self.cam_conf(x) for x in t]),
            fuel=self.fuel.name,
        )


# ═════════════════════════════════════════════════════════ the library ══════

def library(fuel: G.Fuel = G.LPG) -> list[Scenario]:
    F = fuel
    pin = R.orifice_flow_m3s(F, hole_mm=0.5)
    burner_open = R.burner_gas_flow_m3s(F, G.HRR_KW["burner_high"])
    full_bore = R.full_bore_flow_m3s(F, kg_per_h=2.0)

    return [
        # ---------------------------------------------------- benign, sev 0 --
        Scenario("idle", 0, 0, duration_s=1800),

        Scenario("normal_cooking", 1, 0, duration_s=1800,
                 smoke_key=lambda t: "cooking_normal" if t > 120 else "none",
                 steam_g_min=_step(120, 1500, G.STEAM_G_MIN["simmer_covered"]),
                 hrr_kw=_step(120, 1500, G.HRR_KW["burner_low"]),
                 burner_flame=lambda t: 0.9 if 120 <= t < 1500 else 0.0),

        Scenario("steam_uncovered", 2, 0, duration_s=1200,
                 steam_g_min=_step(120, 900, G.STEAM_G_MIN["rice_uncovered"]),
                 hrr_kw=_step(120, 900, G.HRR_KW["burner_low"]),
                 burner_flame=lambda t: 0.9 if 120 <= t < 900 else 0.0),

        Scenario("pressure_cooker", 2, 0, duration_s=900, extractor=False,
                 steam_g_min=_step(200, 400, G.STEAM_G_MIN["pressure_cooker_vent"]),
                 hrr_kw=_step(120, 700, G.HRR_KW["burner_high"]),
                 burner_flame=lambda t: 0.9 if 120 <= t < 700 else 0.0),

        Scenario("frying_tempering", 3, 1, duration_s=1200,
                 smoke_key=lambda t: "frying" if t > 120 else "none",
                 steam_g_min=_step(120, 900, 6.0),
                 co_mg_min=_step(120, 900, G.BURNER_CO_MG_MIN["well_adjusted"]),
                 hrr_kw=_step(120, 900, G.HRR_KW["burner_high"]),
                 burner_flame=lambda t: 0.95 if 120 <= t < 900 else 0.0),

        # A deodorant burst: large, instant, and gone. The single most common
        # false alarm in every MQ build.
        Scenario("aerosol_spike", 0, 0, duration_s=600,
                 gas_src=_step(200, 202, pin * 40.0)),

        # ------------------------------------------------- worth noticing ----
        Scenario("cooking_smoke", 4, 2, duration_s=1200,
                 smoke_key=lambda t: "cooking_smoky" if t > 120 else "none",
                 co_mg_min=_step(120, 800, G.BURNER_CO_MG_MIN["slightly_yellow"]),
                 hrr_kw=_step(120, 800, G.HRR_KW["burner_high"]),
                 burner_flame=lambda t: 0.9 if 120 <= t < 800 else 0.0),

        Scenario("gas_leak_pinhole", 6, 2, duration_s=2400, extractor=False,
                 gas_src=_step(120, 2400, pin), fuel=F,
                 person=lambda t: 0.0),

        # ------------------------------------------------------ hazardous ----
        Scenario("burning_food", 5, 3, duration_s=900,
                 smoke_key=lambda t: "burning_food" if t > 120 else "none",
                 co_mg_min=_step(120, 700, G.BURNER_CO_MG_MIN["badly_yellow"]),
                 hrr_kw=_step(120, 700, G.HRR_KW["burner_high"]),
                 burner_flame=lambda t: 0.9 if 120 <= t < 700 else 0.0,
                 person=lambda t: 0.0 if t > 200 else 1.0),

        Scenario("co_badly_yellow", 7, 3, duration_s=6600, deadline_s=90*60,
                 extractor=False,
                 co_mg_min=_step(120, 6600, G.BURNER_CO_MG_MIN["badly_yellow"]),
                 hrr_kw=_step(120, 6600, G.HRR_KW["burner_high"]),
                 burner_flame=lambda t: 0.9 if t >= 120 else 0.0),

        Scenario("gas_leak_burner_open", 6, 3, duration_s=1800, extractor=False,
                 gas_src=_step(120, 1800, burner_open), fuel=F,
                 person=lambda t: 0.0),

        # --------------------------------------------------------- danger ----
        Scenario("co_starved_enclosed", 7, 4, duration_s=3600, deadline_s=40*60,
                 extractor=False,
                 co_mg_min=_step(120, 3600, G.BURNER_CO_MG_MIN["starved_enclosed"]),
                 hrr_kw=_step(120, 3600, G.HRR_KW["burner_high"]),
                 burner_flame=lambda t: 0.9 if t >= 120 else 0.0,
                 person=lambda t: 0.0),

        Scenario("gas_leak_full_bore", 6, 4, duration_s=900, extractor=False,
                 gas_src=_step(120, 900, full_bore), fuel=F,
                 person=lambda t: 0.0),

        Scenario("pan_oil_ignition", 9, 4, duration_s=600, onset_s=120,
                 deadline_s=180,
                 smoke_key=lambda t: ("oil_pyrolysis" if 120 <= t < 180
                                      else "flaming_fire" if t >= 180 else "none"),
                 co_mg_min=_ramp(180, 240, 0.0, 300.0),
                 hrr_kw=_ramp(180, 260, G.HRR_KW["pan_oil_ignition"],
                              G.HRR_KW["pan_oil_developed"]),
                 open_flame=lambda t: 0.95 if t >= 180 else 0.0,
                 burner_flame=lambda t: 0.9 if t >= 120 else 0.0,
                 person=lambda t: 0.0),

        # ------------------------------------------------ life-threatening ---
        Scenario("gas_plus_ignition", 8, 5, duration_s=600, extractor=False,
                 gas_src=_step(120, 600, full_bore), fuel=F,
                 smoke_key=lambda t: "flaming_fire" if t >= 300 else "none",
                 hrr_kw=_ramp(300, 340, 5.0, G.HRR_KW["pan_oil_developed"]),
                 open_flame=lambda t: 0.95 if t >= 300 else 0.0,
                 onset_s=120, deadline_s=240, person=lambda t: 0.0),

        # ----------------------------------------- degraded-sensing variants --
        # Same fire, camera blinded. The sensor path must carry it alone.
        Scenario("pan_fire_camera_blind", 9, 4, duration_s=600, onset_s=120,
                 deadline_s=180,
                 smoke_key=lambda t: "flaming_fire" if t >= 180 else "none",
                 co_mg_min=_ramp(180, 240, 0.0, 300.0),
                 hrr_kw=_ramp(180, 260, G.HRR_KW["pan_oil_ignition"],
                              G.HRR_KW["pan_oil_developed"]),
                 open_flame=lambda t: 0.0, cam_conf=lambda t: 0.0,
                 person=lambda t: 0.0),

        # Sunlight on an orange cloth: the camera's worst false positive, with
        # no chemistry and no heat behind it. This is the scenario that the
        # cross-modal corroboration exists to kill.
        Scenario("visual_false_positive", 0, 0, duration_s=600,
                 open_flame=lambda t: 0.75 if 200 <= t < 400 else 0.0),
    ]
