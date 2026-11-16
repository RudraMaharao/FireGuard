"""
gas_data.py — the chemistry and the standards. Every number here is either a
physical property, a published exposure limit, or a datasheet-fitted sensor
coefficient. Nothing in this file is tuned.

The three things this file exists to establish:

  1. What concentration is actually dangerous, per published standards, for each
     hazard (fuel gas, carbon monoxide, smoke obscuration, heat).
  2. What a metal-oxide sensor DOES when it meets that concentration.
  3. Why (2) can be trusted at 3.3 V even though the datasheet assumes 5 V.

Point 3 is the whole justification for deriving thresholds analytically, so it
is worth stating precisely.

A MOX sensing element follows a power law over its useful range:

        Rs / R0 = A * C^(-B)

A depends on the element's absolute resistance, which depends on heater
temperature, film thickness, ageing, and the load resistor you happened to
solder on. At 3.3 V instead of 5 V, A is wrong and unknowable.

B is the slope in log-log space. It is set by the surface reaction order — how
many adsorbed oxygen sites a molecule of the target displaces — and it is a
property of the CHEMISTRY, not of the operating point. It shifts by a few
percent with heater temperature, not by a factor.

The pipeline consumes  dev = ln(baseline / ratio)  where ratio = Rs/R0 and
baseline is an EWMA of that same ratio in quiet air. Substituting:

        dev = ln( A*C_base^-B / (A*C^-B) ) = B * ln( C / C_base )

A cancels exactly. The deviation the firmware measures is B times the log of
the concentration ratio, and B is the parameter we can defend. That is why a
physically-derived threshold is meaningful here and an absolute ppm claim is
not.
"""
from __future__ import annotations
from dataclasses import dataclass, field

# ═════════════════════════════════════════════════════════════ fuel gases ═══

@dataclass(frozen=True)
class Fuel:
    name: str
    mw: float               # g/mol, mixture-averaged
    rho_vapour: float       # kg/m^3 at 25 C, 1 atm
    rel_density_air: float  # vs air = 1.0 -> decides where it collects
    lel_vol: float          # lower explosive limit, volume fraction
    hhv_mj_kg: float        # gross calorific value
    reg_pressure_pa: float  # domestic regulator outlet
    mq2_fit_a: float        # ppm = a * (Rs/R0)^(-n)   [datasheet log-log fit]
    mq2_fit_n: float
    stratification: float   # near-sensor / well-mixed concentration ratio

    @property
    def mq2_B(self) -> float:
        """Power-law exponent B in Rs/R0 = A*C^-B. This is 1/n."""
        return 1.0 / self.mq2_fit_n

    @property
    def c_base_ppm(self) -> float:
        """Apparent clean-air concentration: what the fit reads at Rs/R0 = 1.
        This is the C_base that the deviation formula divides by."""
        return self.mq2_fit_a

    @property
    def lel_ppm(self) -> float:
        return self.lel_vol * 1e6


# Indian domestic LPG per IS 4576: predominantly butane with propane, the ratio
# varies seasonally and by bottler. 60/40 butane/propane by mass is the usual
# working assumption. LEL of the mixture sits between propane's 2.1 % and
# butane's 1.8 %.
LPG = Fuel(
    name="LPG",
    mw=52.5,                    # 0.6*58.12 + 0.4*44.10, mass-weighted
    rho_vapour=2.12,
    rel_density_air=1.83,       # SINKS. Pools at floor level.
    lel_vol=0.019,              # 1.9 % v/v = 19 000 ppm
    hhv_mj_kg=46.1,
    reg_pressure_pa=2800.0,     # 28 mbar, IS 9798 domestic low-pressure reg
    mq2_fit_a=574.25,           # standard MQ-2 LPG log-log fit
    mq2_fit_n=2.222,
    stratification=2.2,         # denser than air; sensor low = reads high
)

# Piped natural gas: essentially methane. Lighter than air, so it does the
# opposite — it collects at the ceiling and a floor-mounted sensor under-reads.
PNG = Fuel(
    name="PNG",
    mw=16.9,                    # ~96 % CH4 with some ethane/N2
    rho_vapour=0.68,
    rel_density_air=0.58,       # RISES. Collects at ceiling.
    lel_vol=0.050,              # 5.0 % v/v = 50 000 ppm
    hhv_mj_kg=52.2,
    reg_pressure_pa=2100.0,     # 21 mbar typical Indian CGD service pressure
    mq2_fit_a=658.71,           # MQ-2 methane fit
    mq2_fit_n=2.168,
    stratification=1.8,         # sensor mounted HIGH for PNG
)

FUELS = {"LPG": LPG, "PNG": PNG}


# ══════════════════════════════════════════════════ other sensing channels ══

@dataclass(frozen=True)
class Channel:
    name: str
    fit_a: float
    fit_n: float

    @property
    def B(self) -> float:
        return 1.0 / self.fit_n

    @property
    def c_base_ppm(self) -> float:
        return self.fit_a


# MQ-7 answers to carbon monoxide, but without the 60 s / 90 s heater cycle it
# also answers to hydrogen and to LPG. The fit is still the right shape; the
# selectivity is what is missing, which is why the firmware uses the MQ-7 minus
# MQ-2 DIFFERENTIAL rather than MQ-7 alone.
MQ7_CO = Channel("MQ7_CO", fit_a=99.042, fit_n=1.518)

# MQ-135's published fits are for CO2/NH3/benzene. For combustion products it is
# best read as a broadband "something is oxidising in here" channel; the CO2 fit
# gives the right slope for that purpose.
MQ135_AIR = Channel("MQ135_AIR", fit_a=110.47, fit_n=2.862)

# MQ-2's smoke fit — particulates and pyrolysis volatiles, distinct from its LPG
# fit and with a shallower slope.
MQ2_SMOKE = Channel("MQ2_SMOKE", fit_a=3616.1, fit_n=2.675)


# ═══════════════════════════════════════════════════════ exposure limits ════

# EN 50291-1 domestic CO alarm response envelope. The alarm MUST sound inside
# the upper bound and MUST NOT sound before the lower bound — the lower bound is
# there to prevent nuisance alarms from a gas hob's ordinary output.
CO_ALARM_ENVELOPE = {
    #  ppm : (must_not_alarm_before_s, must_alarm_within_s)
    30:  (120 * 60, None),        # 30 ppm: must not alarm inside 120 min
    50:  (60 * 60, 90 * 60),
    100: (10 * 60, 40 * 60),
    300: (None, 3 * 60),          # 300 ppm: 3 minutes, no lower bound
}

# WHO indoor air quality guidelines for CO (2010), as concentration ceilings.
CO_WHO = {"15min": 87.0, "1h": 26.0, "8h": 8.6}   # ppm

# IS/IEC 60079-29-1 and general industrial practice: a combustible-gas detector
# alarms at a fraction of LEL. 10 % LEL is the near-universal first alarm.
GAS_ALARM_FRAC_LEL_LOW = 0.10     # first alarm
GAS_ALARM_FRAC_LEL_HIGH = 0.25    # second alarm / evacuate

# Smoke obscuration, %/m. EN 54-7 / UL 217 domestic photoelectric alarms are
# type-tested to alarm between roughly 2 and 8 %/m. Cooking routinely produces
# 1-3 %/m at the hob, which is exactly why kitchen smoke alarms get muted.
SMOKE_OBSCURATION = {
    "cooking_normal": 0.6,
    "cooking_smoky": 2.5,
    "burning_food": 6.0,
    "flaming_fire": 14.0,
    "alarm_floor_en54": 2.0,
    "alarm_ceiling_en54": 8.0,
}

# Oil smoke points, deg C. Above these, pyrolysis volatiles climb steeply; the
# autoignition point is another 100-150 K further up.
OIL_SMOKE_POINT_C = {
    "mustard": 250.0, "ghee": 250.0, "refined_sunflower": 227.0,
    "groundnut": 232.0, "coconut": 177.0,
}
OIL_AUTOIGNITION_C = 363.0        # typical for vegetable oils


# ════════════════════════════════════════════════ combustion source terms ═══

# CO emission from a domestic gas burner, mg/min. A correctly adjusted burner
# with a blue flame emits very little; a yellow-tipping, lint-blocked or
# oxygen-starved burner is the dangerous case and is the reason MQ-7 is here.
BURNER_CO_MG_MIN = {
    "well_adjusted": 3.0,
    "slightly_yellow": 18.0,
    "badly_yellow": 55.0,
    "starved_enclosed": 160.0,
}

# Water vapour released by open cooking, g/min. This is the quantity that makes
# steam separable from smoke: it moves ABSOLUTE humidity hard while barely
# moving temperature.
STEAM_G_MIN = {
    "kettle_open": 22.0, "rice_uncovered": 11.0, "pressure_cooker_vent": 35.0,
    "simmer_covered": 3.0,
}

# Heat release rates, kW. Used for the thermal channel and for the plume model.
HRR_KW = {
    "burner_low": 0.9, "burner_high": 2.8, "two_burners": 5.0,
    "pan_oil_ignition": 12.0, "pan_oil_developed": 35.0,
    "tea_light": 0.08, "paper_towel": 3.0,
}

AIR_RHO = 1.184          # kg/m^3 at 25 C
AIR_CP = 1005.0          # J/(kg K)
MOLAR_VOL_25C = 24.45    # L/mol at 25 C, 1 atm
