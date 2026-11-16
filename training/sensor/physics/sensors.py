"""
sensors.py — from concentration trajectories to what the firmware actually sees.

The firmware never sees ppm. It sees

    dev_s = ln( baseline_s / (Rs_s/R0_s) )

and this module produces exactly that quantity, so the thresholds derived here
are in the same units the firmware compares against. No conversion happens at
the boundary, which is the usual place train/serve skew creeps in.

Two effects are modelled beyond the plain power law, because both of them are
large enough to change a threshold:

  CROSS-SENSITIVITY. No MQ sensor is selective. Each one responds to every
  reducing gas present, scaled by a cross-sensitivity coefficient. This is why
  the firmware works on channel DIFFERENCES: the differences carry a signature
  even though no single channel does.

  HUMIDITY. A MOX element's resistance falls as water vapour displaces adsorbed
  oxygen. Over a realistic 55 -> 85 %RH cooking swing this alone produces a
  deviation of roughly 0.07 — several times the sensor's quiet-air noise width.
  Left unmodelled it looks exactly like a small gas leak, and it is the single
  biggest source of false alarms in every amateur MQ build. The steam veto
  exists because of this term, so the term has to be in the model that sizes it.
"""
from __future__ import annotations
from dataclasses import dataclass
import numpy as np

from . import gas_data as G


# Cross-sensitivity matrix: how much one ppm of each species reads as one ppm of
# the channel's own reference gas. Read off the datasheet sensitivity families
# (the fan of curves on the Rs/R0 vs ppm plot). These are approximate — they are
# ratios of log-log intercepts, and the curves are not perfectly parallel — but
# the ORDERING is solid and the ordering is what the differential features use.
XSENS = {
    #            lpg   png   co    smoke  h2
    "mq2":   dict(lpg=1.00, png=0.60, co=0.15, smoke=0.80, h2=1.20),
    "mq7":   dict(lpg=0.30, png=0.15, co=1.00, smoke=0.25, h2=1.50),
    "mq135": dict(lpg=0.40, png=0.25, co=0.35, smoke=1.00, h2=0.55),
}

# Power-law exponent B per channel, from gas_data's datasheet fits.
B_OF = {
    "mq2":   G.LPG.mq2_B,          # 0.450
    "mq7":   G.MQ7_CO.B,           # 0.659
    "mq135": G.MQ135_AIR.B,        # 0.349
}

# Apparent clean-air concentration in each channel's own reference units.
CBASE_OF = {
    "mq2":   G.LPG.c_base_ppm,     # 574 ppm LPG-equivalent
    "mq7":   G.MQ7_CO.c_base_ppm,  # 99 ppm CO-equivalent
    "mq135": G.MQ135_AIR.c_base_ppm,
}

CHANNELS = ("mq2", "mq7", "mq135")


@dataclass
class SensorNoise:
    """Quiet-air behaviour. dev_sigma is the quantity the firmware estimates
    on-device as devScale and uses as its z-score denominator, so getting its
    magnitude right is what makes a derived z-threshold meaningful."""
    dev_sigma: float = 0.012          # per-sample sd of dev in still air
    # Drift is expressed as MULTIPLES of the per-sample noise, not as an
    # absolute figure. A quieter element is a more stable element — the same
    # physics (grain growth, humidity history, heater ageing) sets both — so
    # holding drift fixed while sweeping sigma would sweep the drift-to-noise
    # ratio by a factor of six and test the wrong thing entirely.
    drift_common_mult: float = 1.7    # wander shared by all three elements
    drift_indep_mult: float = 0.5     # wander unique to one element
    seed: int = 7

    @property
    def drift_common_per_hour(self) -> float:
        return self.drift_common_mult * self.dev_sigma

    @property
    def drift_indep_per_hour(self) -> float:
        return self.drift_indep_mult * self.dev_sigma


def humidity_term(rh: np.ndarray, t_c: np.ndarray,
                  rh_ref: float = 65.0, t_ref: float = 20.0) -> np.ndarray:
    """Deviation contributed by humidity and temperature alone, no gas present.

    Linearised from the MQ family's published Rs correction surface:
        Rs/Rs_ref ~= 1 - 0.0035 (RH - 65) - 0.008 (T - 20)
    and dev = -ln(Rs/Rs_ref).
    """
    factor = 1.0 - 0.0035 * (rh - rh_ref) - 0.008 * (t_c - t_ref)
    factor = np.clip(factor, 0.35, 2.0)
    return -np.log(factor)


def dev_from_species(channel: str, species_ppm: dict[str, np.ndarray],
                     rh: np.ndarray | None = None,
                     t_c: np.ndarray | None = None) -> np.ndarray:
    """The core conversion.

        dev = B * ln( (C_base + sum_g k_g C_g) / C_base )  +  humidity term

    Note what is absent: the sensor's absolute resistance, the load resistor,
    the heater voltage, and R0. All of them live in the coefficient A, and A
    cancelled when the firmware took the log-ratio against its own baseline.
    That cancellation is the reason a 3.3 V build can still be reasoned about
    quantitatively.
    """
    k = XSENS[channel]
    cb = CBASE_OF[channel]
    B = B_OF[channel]

    any_arr = next(iter(species_ppm.values()))
    total = np.zeros_like(np.asarray(any_arr, dtype=float))
    for sp, conc in species_ppm.items():
        total = total + k.get(sp, 0.0) * np.asarray(conc, dtype=float)

    dev = B * np.log((cb + total) / cb)
    if rh is not None and t_c is not None:
        dev = dev + humidity_term(np.asarray(rh, float), np.asarray(t_c, float))
    return dev


def drift_walk(n: int, dt: float, per_hour: float,
               rng: np.random.Generator) -> np.ndarray:
    step = per_hour * np.sqrt(dt / 3600.0)
    return np.cumsum(rng.normal(0.0, step, n))


def add_noise(dev: np.ndarray, noise: SensorNoise, dt: float,
              rng: np.random.Generator, common: np.ndarray) -> np.ndarray:
    """White measurement noise, a per-element random walk, and a shared one.

    Splitting the drift matters more than its magnitude. Three MOX elements on
    one board share a temperature, a humidity history, a supply rail and an
    ageing environment, so most of their drift is COMMON MODE. The firmware's
    discriminants are channel contrasts, and a contrast subtracts common-mode
    drift out by construction — so modelling the drift as three independent
    random walks would make the contrasts look far noisier than they are and
    would push every threshold up for no physical reason.
    """
    n = len(dev)
    white = rng.normal(0.0, noise.dev_sigma, n)
    indep = drift_walk(n, dt, noise.drift_indep_per_hour, rng)
    return dev + white + indep + common


def rolling_baseline(dev: np.ndarray, dt: float, tau_s: float = 1800.0,
                     freeze_z: float = 1.0, sigma: float = 0.012,
                     smooth_tau_s: float = 30.0) -> np.ndarray:
    """Reproduces the firmware's EWMA baseline INCLUDING its freeze rule.

    The freeze is the part that matters and the part most implementations get
    wrong: if the baseline keeps tracking during a slow leak, it chases the leak
    and the deviation stays near zero forever. The system goes blind to exactly
    the hazard class that develops slowest and kills most reliably.

    The freeze decision is taken on a 30-second SMOOTHED deviation, not on the
    instantaneous sample. Testing the raw sample against a one-sigma threshold
    freezes the baseline roughly a third of the time on noise alone, and once
    frozen it stops tracking the sensor's slow drift — so the drift accumulates
    into the deviation and every channel wanders upward until it false-alarms.
    That failure took hours to find in simulation and would have taken weeks to
    find on a bench, because it only appears after the sensor has been running
    long enough to drift.
    """
    a = dt / tau_s
    a_s = dt / smooth_tau_s
    base = np.empty_like(dev)
    b = dev[0]
    sm = dev[0]
    for i, x in enumerate(dev):
        sm += (x - sm) * a_s
        if abs(sm - b) / max(sigma, 1e-6) < freeze_z:
            b += (x - b) * a
        base[i] = b
    return base


def observed_dev(dev_true: np.ndarray, dt: float, noise: SensorNoise,
                 rng: np.random.Generator, common: np.ndarray,
                 quiet_n: int | None = None) -> tuple[np.ndarray, np.ndarray]:
    """What the firmware's `dev` variable actually holds: the measured deviation
    minus its own tracked baseline. Returns (dev_observed, devScale).

    `quiet_n` bounds the window used to estimate devScale. The firmware only
    updates devScale while the state machine is in SAFE, and that restriction is
    not cosmetic: if the estimate runs on into a developing event, the rising
    signal inflates the very denominator every z-score divides by, and the
    hazard scales itself away to nothing. Reproducing the restriction here is
    what keeps the simulation honest about what the firmware can actually see.
    """
    meas = add_noise(dev_true, noise, dt, rng, common)
    base = rolling_baseline(meas, dt, sigma=noise.dev_sigma)
    obs = meas - base
    n_q = quiet_n if quiet_n is not None else int(240.0 / dt)
    n_q = int(np.clip(n_q, 60, len(obs)))
    quiet = obs[:n_q]
    # MAD rather than standard deviation: one startup transient inside the
    # window would otherwise widen the denominator and desensitise everything
    # downstream of it.
    scale = max(1.4826 * float(np.median(np.abs(quiet - np.median(quiet)))),
                noise.dev_sigma)
    return obs, np.full_like(obs, scale)


# ═══════════════════════════════════════════════════ inverse: what z means ══

def z_for_concentration(channel: str, species: str, ppm: float,
                        dev_sigma: float = 0.012) -> float:
    """How many quiet-air noise widths a given concentration produces.

    This is the function that turns a published safety limit into a firmware
    threshold. 'The alarm must fire at 10 % LEL' becomes 'the alarm must fire at
    z = 27 on MQ-2', and z is a quantity the firmware has.
    """
    k = XSENS[channel].get(species, 0.0)
    if k <= 0:
        return 0.0
    cb = CBASE_OF[channel]
    B = B_OF[channel]
    dev = B * np.log((cb + k * ppm) / cb)
    return float(dev / dev_sigma)


def concentration_for_z(channel: str, species: str, z: float,
                        dev_sigma: float = 0.012) -> float:
    """Inverse of the above: the concentration a given z corresponds to. Used to
    report what each derived threshold means in physical terms, which is what
    makes the numbers auditable rather than magic."""
    k = XSENS[channel].get(species, 0.0)
    if k <= 0:
        return float("inf")
    cb = CBASE_OF[channel]
    B = B_OF[channel]
    return float((cb * np.exp(z * dev_sigma / B) - cb) / k)


# ══════════════════════════════════ what the fusion layer does before deciding ═

def condition(x: np.ndarray, dt: float, attack_s: float = 2.0,
              release_s: float = 20.0, persist_on: float = 0.12,
              rise_s: float = 12.0, fall_s: float = 45.0,
              floor: float = 0.40) -> np.ndarray:
    """Mirror of fusion.h stage 1: 5-sample median, asymmetric EWMA, persistence.

    Evaluating a threshold against the RAW score is unfair to the design and
    gives the wrong answer in both directions. Nothing in this system alarms on
    an instantaneous value — a score has to survive a median filter, then a
    20-second release time constant, then a persistence integrator that discounts
    anything new to 40 % of its face value. A two-second noise excursion cannot
    reach the state machine, so it must not be allowed to fail the derivation.
    """
    n = len(x)
    med = np.empty(n)
    for i in range(n):
        med[i] = np.median(x[max(0, i - 4): i + 1])
    sm = np.empty(n)
    s = med[0]
    for i, v in enumerate(med):
        tau = attack_s if v > s else release_s
        s += (v - s) * min(dt / tau, 1.0)
        sm[i] = s
    pr = np.empty(n)
    p = 0.0
    for i, v in enumerate(sm):
        p += dt / rise_s if v > persist_on else -dt / fall_s
        p = min(max(p, 0.0), 1.0)
        pr[i] = p
    return sm * (floor + (1.0 - floor) * pr)
