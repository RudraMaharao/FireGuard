"""
room.py — the well-stirred reactor with a stratification correction.

A kitchen is not well mixed, but over the 30-second to 10-minute timescales this
system cares about, a single-zone mass balance plus a stratification factor gets
concentration trajectories right to within a factor of about two — which is far
better than the sensor's own absolute accuracy at 3.3 V, so it is not the
limiting error.

    V dC/dt = S(t) - (Q_vent + Q_dep) C

    dC/dt = S(t)/V - (lambda_vent + lambda_dep) C

lambda_vent = ACH/3600 [1/s]. lambda_dep is surface deposition, which matters
only for particulates; gases have effectively zero deposition on this timescale.

Temperature and humidity get the same treatment with their own source terms and
an extra loss path to the walls, which is what stops a small fire from heating a
kitchen adiabatically in the model when it does not in reality.
"""
from __future__ import annotations
from dataclasses import dataclass, field
import numpy as np

from . import gas_data as G


@dataclass
class Room:
    """A kitchen. Defaults are the user's: medium, well ventilated."""
    volume_m3: float = 25.0
    ach_closed: float = 2.0          # windows shut, extractor off
    ach_extractor: float = 8.0       # extractor running / window open
    wall_loss_w_per_k: float = 42.0  # convective+conductive loss to structure
    t_ambient_c: float = 28.0        # Indian kitchen, year-round
    rh_ambient_pct: float = 55.0

    # Where the sensor sits relative to where the hazard collects. 1.0 means the
    # sensor reads the well-mixed value; >1 means it sits in the accumulating
    # layer and reads high; <1 means it sits in the wrong layer entirely.
    sensor_height_m: float = 1.2
    ceiling_m: float = 2.7

    def lam(self, extractor: bool) -> float:
        ach = self.ach_extractor if extractor else self.ach_closed
        return ach / 3600.0

    # ---------------------------------------------------------------- gases --
    def stratification_factor(self, rel_density_air: float) -> float:
        """How much the sensor over- or under-reads because the gas is not
        neutrally buoyant. A crude two-layer argument: a gas denser than air
        occupies the lower fraction of the room, concentrating there.

        Returns sensor_concentration / well_mixed_concentration.
        """
        h = np.clip(self.sensor_height_m / self.ceiling_m, 0.05, 0.95)
        if abs(rel_density_air - 1.0) < 0.05:
            return 1.0
        if rel_density_air > 1.0:
            # Heavier: fills from the floor up. Effective layer thickness grows
            # with the density excess; a sensor inside it reads 1/frac.
            frac = np.clip(1.0 / rel_density_air, 0.25, 0.95)
            return (1.0 / frac) if h <= frac else 0.35
        else:
            # Lighter: fills from the ceiling down.
            frac = np.clip(rel_density_air, 0.25, 0.95)
            top = 1.0 - frac
            return (1.0 / frac) if h >= top else 0.35


def integrate(room: Room, dt: float, n: int,
              source_fn, lam_fn, c0: float = 0.0,
              lam_dep: float = 0.0) -> np.ndarray:
    """Explicit trapezoidal integration of the single-zone balance.

    source_fn(t) -> emission in m^3/s (gases) or g/s (vapour/particulate)
    lam_fn(t)    -> air-change rate in 1/s
    Returns concentration as a volume fraction (gases) or g/m^3.
    """
    c = np.empty(n)
    x = c0
    for i in range(n):
        t = i * dt
        s = source_fn(t) / room.volume_m3
        lam = lam_fn(t) + lam_dep
        # trapezoidal on the linear decay term keeps this stable at dt = 0.5 s
        # for every lambda this model produces
        k = np.exp(-lam * dt)
        x = x * k + (s / lam) * (1.0 - k) if lam > 1e-9 else x + s * dt
        c[i] = x
    return c


def thermal(room: Room, dt: float, n: int, hrr_fn, lam_fn,
            coupling: float = 0.06) -> np.ndarray:
    """Room air temperature rise above ambient, in K.

    `coupling` is the fraction of the fire's heat release that actually reaches
    the sensor's air parcel rather than going up the extractor, into the pan, or
    radiating to surfaces. For a hob-adjacent sensor 4-8 % is the realistic band;
    the rest of the enthalpy never becomes measurable room-air temperature.
    That single factor is what keeps a 12 kW pan fire from predicting an
    implausible 40 K jump.
    """
    m_air = room.volume_m3 * G.AIR_RHO
    dT = np.empty(n)
    x = 0.0
    for i in range(n):
        t = i * dt
        q_in = coupling * hrr_fn(t) * 1000.0                 # W
        q_out = (lam_fn(t) * m_air * G.AIR_CP + room.wall_loss_w_per_k) * x
        x += (q_in - q_out) / (m_air * G.AIR_CP) * dt
        dT[i] = max(x, 0.0)
    return dT


def abs_humidity(t_c: np.ndarray | float, rh_pct: np.ndarray | float) -> np.ndarray | float:
    """Absolute humidity, g/m^3, from the Magnus formula. Mirrors the firmware's
    absHumidity() in sensor_features.h exactly."""
    return (6.112 * np.exp(17.67 * t_c / (t_c + 243.5))
            * rh_pct * 2.1674 / (273.15 + t_c))


def rh_from_abs(ah: np.ndarray | float, t_c: np.ndarray | float) -> np.ndarray | float:
    """Inverse of the above — needed because a fire raises T at nearly constant
    absolute humidity, which DROPS relative humidity. That inversion is the
    cleanest single discriminator between a fire and a pot of boiling water."""
    return ah * (273.15 + t_c) / (6.112 * np.exp(17.67 * t_c / (t_c + 243.5)) * 2.1674)


def humidity(room: Room, dt: float, n: int, steam_g_s_fn, lam_fn,
             dT: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Returns (absolute humidity g/m^3, relative humidity %) trajectories.

    Steam sources add water. Temperature rise does not add water — it only
    changes what that water means in relative terms. Both are returned because
    the firmware logs RH (what the BME280 reports) but reasons about AH.
    """
    ah0 = abs_humidity(room.t_ambient_c, room.rh_ambient_pct)
    # Integrate the EXCESS over ambient. Ventilation pulls the room back toward
    # outdoor humidity, not toward zero, so the baseline is added afterwards
    # rather than carried through the decay term.
    excess = integrate(room, dt, n, steam_g_s_fn, lam_fn, c0=0.0)
    ah = ah0 + excess
    t = room.t_ambient_c + dT
    rh = np.clip(rh_from_abs(ah, t), 1.0, 100.0)
    return ah, rh


# ═════════════════════════════════════════════════════════ source builders ══

def orifice_flow_m3s(fuel: G.Fuel, hole_mm: float, cd: float = 0.62) -> float:
    """Volumetric leak rate through a small hole at regulator pressure.

    Incompressible orifice equation. At 2.8 kPa the pressure ratio is 1.028, far
    below the 1.9 critical ratio, so the flow is subsonic and the incompressible
    form is accurate to a few percent — no need for the choked-flow treatment.
    """
    a = np.pi * (hole_mm * 1e-3 / 2.0) ** 2
    v = np.sqrt(2.0 * fuel.reg_pressure_pa / fuel.rho_vapour)
    return cd * a * v


def burner_gas_flow_m3s(fuel: G.Fuel, kw: float) -> float:
    """Fuel flow for an UNLIT burner left open at a given nominal power."""
    kg_s = kw / (fuel.hhv_mj_kg * 1000.0)
    return kg_s / fuel.rho_vapour


def full_bore_flow_m3s(fuel: G.Fuel, kg_per_h: float = 2.0) -> float:
    """Hose off the regulator: the regulator's own rated delivery. Domestic
    Indian regulators are rated 1.5-3 kg/h."""
    return (kg_per_h / 3600.0) / fuel.rho_vapour


def co_flow_m3s(mg_per_min: float) -> float:
    """CO emission converted to volumetric flow at 25 C."""
    g_s = mg_per_min / 1000.0 / 60.0
    mol_s = g_s / 28.01
    return mol_s * G.MOLAR_VOL_25C / 1000.0
