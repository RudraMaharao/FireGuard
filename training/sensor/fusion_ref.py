"""
fusion_ref.py — offline mirror of firmware/sensor_node/fusion.h.

Two jobs:
  1. `python fusion_ref.py` runs the 16-scenario regression suite. Any change to
     a coefficient that breaks the required ordering fails here, which is what
     stops the risk score quietly drifting as you tune other things.
  2. calibrate_fusion.py imports FusionEngine to replay recorded logs offline.

The replay MUST reproduce the on-device FRS trace to within about +/-1 point.
If it does not, fix that before tuning anything — you cannot calibrate through
a discrepancy between the thing you are fitting and the thing that runs.
"""
from __future__ import annotations
import numpy as np

CHAN = ["Fv", "Sg", "Sv", "Cg", "Te", "Gg"]

DEFAULTS = dict(
    # severity ceilings: max FRS this channel can produce alone
    c_Fv=0.85, c_Sg=0.45, c_Sv=0.40, c_Cg=0.38, c_Te=0.30, c_Gg=0.30,
    # corroboration bonuses
    k_Fv_Gg=25.0, k_Fv_Sg=12.0, k_Sv_Sg=10.0, k_Sg_Te=7.0,
    k_Cg_Sg=6.0, k_Fv_Sv=6.0, k_Sv_Te=5.0, k_Gg_Cg=5.0, k_triple=18.0,
    # context suppression
    d_cook=0.35, d_steam=0.25, d_floor=0.55,
    # temporal conditioning
    attack_s=2.0, release_s=20.0,
    persist_on=0.12, persist_rise_s=12.0, persist_fall_s=45.0, persist_floor=0.40,
)

# Fittable parameters and their bounds, for calibrate_fusion.py
BOUNDS = {
    "c_Fv": (0.70, 0.95), "c_Sg": (0.30, 0.60), "c_Sv": (0.25, 0.55),
    "c_Cg": (0.25, 0.50), "c_Te": (0.15, 0.45), "c_Gg": (0.20, 0.45),
    "k_Fv_Gg": (15, 35), "k_Fv_Sg": (5, 20), "k_Sv_Sg": (4, 18),
    "k_Sg_Te": (2, 14), "k_Cg_Sg": (2, 12), "k_triple": (8, 28),
    "d_cook": (0.20, 0.50), "d_steam": (0.10, 0.40),
    "persist_rise_s": (6.0, 20.0), "persist_floor": (0.25, 0.55),
}


class FusionEngine:
    """Stateful. Feed it 2 Hz samples in order; it carries the same median,
    EWMA and persistence state the firmware does."""

    def __init__(self, **over):
        self.p = dict(DEFAULTS)
        self.p.update(over)
        self.reset()

    def reset(self):
        n = len(CHAN)
        self.med = np.zeros((n, 5))
        self.mi = 0
        self.sm = np.zeros(n)
        self.pr = np.zeros(n)
        self.slope = []
        self.prev_state = "SAFE"

    # ---------------------------------------------------------- stage 1 ----
    def condition(self, raw, dt):
        p = self.p
        self.med[:, self.mi] = raw
        self.mi = (self.mi + 1) % 5
        x = np.median(self.med, axis=1)

        tau = np.where(x > self.sm, p["attack_s"], p["release_s"])
        self.sm += (x - self.sm) * np.minimum(dt / tau, 1.0)

        up = self.sm > p["persist_on"]
        self.pr += np.where(up, dt / p["persist_rise_s"], -dt / p["persist_fall_s"])
        self.pr = np.clip(self.pr, 0.0, 1.0)

        pf = p["persist_floor"]
        return self.sm * (pf + (1.0 - pf) * self.pr)

    # -------------------------------------------------------- stages 2-4 ---
    def aggregate(self, cond, w, k_cook=0.0, k_steam=0.0, dt=0.5):
        p = self.p
        e = np.clip(np.asarray(w, float) * cond, 0, 1)
        c = np.array([p["c_Fv"], p["c_Sg"], p["c_Sv"], p["c_Cg"], p["c_Te"], p["c_Gg"]])

        s_base = 100.0 * (1.0 - np.prod(1.0 - c * e))

        Fv, Sg, Sv, Cg, Te, Gg = e
        b = (p["k_Fv_Gg"] * Fv * Gg + p["k_Fv_Sg"] * Fv * Sg + p["k_Sv_Sg"] * Sv * Sg
             + p["k_Sg_Te"] * Sg * Te + p["k_Cg_Sg"] * Cg * Sg + p["k_Fv_Sv"] * Fv * Sv
             + p["k_Sv_Te"] * Sv * Te + p["k_Gg_Cg"] * Gg * Cg
             + p["k_triple"] * Gg * max(Sv, Sg) * Fv)

        d = float(np.clip(1.0 - p["d_cook"] * k_cook - p["d_steam"] * k_steam,
                          p["d_floor"], 1.0))
        s = (s_base + b) * d

        # hard safety floors, applied AFTER damping so context cannot mute them
        if Fv >= 0.80:                                   s = max(s, 75.0)
        if Fv >= 0.80 and (Sg >= 0.50 or Sv >= 0.60):    s = max(s, 88.0)
        if Gg >= 0.85 and Fv >= 0.50:                    s = max(s, 90.0)
        if Cg >= 0.85:                                   s = max(s, 55.0)

        self.slope.append(s)
        if len(self.slope) > 6:
            self.slope.pop(0)
        if len(self.slope) == 6 and (s - self.slope[0]) / (6 * dt) > 8.0:
            s += 10.0

        frs = float(np.clip(s, 0, 100))
        lpg = Gg > 0.5
        grs = float(np.clip(100 * Gg * (1 + 0.10 * Cg) + (12 * Gg if lpg else 0), 0, 100))
        crs = float(np.clip(100 * Cg * (1 + 0.10 * Te), 0, 100))
        return frs, grs, crs, s_base, b, d, e

    def step(self, raw, w=None, k_cook=0.0, k_steam=0.0, dt=0.5):
        w = np.ones(6) if w is None else w
        cond = self.condition(np.asarray(raw, float), dt)
        return self.aggregate(cond, w, k_cook, k_steam, dt)


def state_of(frs, grs, crs, Fv_e):
    """Priority arbitration. Visual confirmation gates FIRE DETECTED and
    CRITICAL unless the score alone is extreme."""
    flame = Fv_e >= 0.60
    if (frs >= 90 and flame and grs >= 70) or frs >= 96:  return "CRITICAL"
    if (frs >= 70 and flame) or frs >= 92:                return "FIRE DETECTED"
    if grs >= 70:                                         return "GAS LEAK DETECTED"
    if crs >= 65:                                         return "CO HAZARD"
    if frs >= 60:                                         return "FIRE SUSPECTED"
    if frs >= 45:                                         return "SMOKE WARNING"
    if grs >= 40:                                         return "GAS LEAK SUSPECT"
    if frs >= 25:                                         return "WARNING"
    if frs >= 10:                                         return "NORMAL COOKING"
    return "SAFE"


# ======================================================== regression suite ==
# Steady-state evidence vectors. Held long enough for persistence to saturate,
# so these are the equilibrium scores each scenario settles at.
SCEN = [
    # name                                    Fv    Sg    Sv    Cg    Te    Gg  cook steam  band
    ("Empty kitchen, idle",                 0.00, 0.03, 0.02, 0.02, 0.05, 0.02, 0.0, 0.0, (0, 10)),
    ("Normal cooking, sensors clean",       0.00, 0.15, 0.35, 0.08, 0.20, 0.05, 1.0, 0.0, (10, 25)),
    ("Boiling kettle, heavy steam",         0.00, 0.08, 0.30, 0.05, 0.45, 0.05, 0.3, 1.0, (10, 30)),
    ("Oil and spice frying fumes",          0.02, 0.35, 0.45, 0.15, 0.30, 0.10, 0.8, 0.0, (20, 45)),
    ("Cooking smoke, moderate",             0.02, 0.40, 0.55, 0.15, 0.30, 0.08, 0.5, 0.0, (25, 50)),
    ("Burning food, strong smoke",          0.05, 0.62, 0.70, 0.30, 0.50, 0.10, 0.0, 0.0, (60, 85)),
    ("Strong smoke + rising temperature",   0.08, 0.75, 0.80, 0.35, 0.70, 0.10, 0.0, 0.0, (75, 95)),
    ("Slow small LPG leak, no flame",       0.00, 0.10, 0.05, 0.10, 0.05, 0.55, 0.0, 0.0, (15, 40)),
    ("Major LPG leak, no flame",            0.00, 0.15, 0.05, 0.20, 0.05, 0.90, 0.0, 0.0, (30, 55)),
    ("CO buildup, badly burning flame",     0.02, 0.40, 0.20, 0.85, 0.30, 0.25, 0.0, 0.0, (50, 75)),
    ("Visible flame only, candle-scale",    0.90, 0.10, 0.15, 0.05, 0.15, 0.05, 0.0, 0.0, (70, 90)),
    ("Gas leak + smoke, no flame",          0.05, 0.70, 0.65, 0.35, 0.35, 0.80, 0.0, 0.0, (75, 95)),
    ("Gas leak + visible flame",            0.88, 0.25, 0.20, 0.30, 0.30, 0.85, 0.0, 0.0, (90, 100)),
    ("Gas + smoke + flame, worst case",     0.92, 0.82, 0.85, 0.50, 0.65, 0.88, 0.0, 0.0, (95, 100)),
    ("CNN false positive (dim, w=0.35)",    0.55, 0.05, 0.10, 0.03, 0.05, 0.03, 0.0, 0.0, (0, 30)),
    # TRANSIENT: raw value held for only 2 s, then released. This is the real
    # anti-false-positive test - a deodorant burst reads high for a moment.
    ("2 s aerosol spike (transient)",        0.00, 0.55, 0.05, 0.20, 0.05, 0.70, 0.0, 0.0, (0, 30)),
]
TRANSIENT = {15: 2.0}          # scenario index -> seconds the evidence is present

# Required ordering: peak FRS must be non-decreasing along this chain.
ORDER = [0, 1, 3, 4, 5, 6, 12, 13]


def run_suite(verbose=True, **over):
    rows, fail = [], 0
    for (name, Fv, Sg, Sv, Cg, Te, Gg, kc, ks, band) in SCEN:
        w = np.ones(6)
        if "false positive" in name:
            w[0] = w[2] = 0.35          # dim scene: the CNN says so itself
        eng = FusionEngine(**over)
        raw = np.array([Fv, Sg, Sv, Cg, Te, Gg], float)
        idx = len(rows)
        hold = TRANSIENT.get(idx)
        peak = 0.0; snap = None
        n = 120 if hold is None else 100
        for t in range(n):                       # 50-60 s
            live = raw if (hold is None or t * 0.5 < hold) else raw * 0.0
            frs, grs, crs, base, b, d, e = eng.step(live, w, kc, ks)
            if frs >= peak:
                peak = frs; snap = (grs, crs, base, b, d, e)
        grs, crs, base, b, d, e = snap
        frs = peak
        st = state_of(frs, grs, crs, e[0])
        ok = band[0] <= frs <= band[1]
        fail += not ok
        rows.append((name, frs, grs, crs, base, b, d, st, ok))

    peaks = [rows[i][1] for i in ORDER]
    mono = all(peaks[i] <= peaks[i + 1] + 1e-6 for i in range(len(peaks) - 1))

    if verbose:
        print(f"{'scenario':<38}{'peak':>5}{'base':>7}{'syn':>7}{'damp':>6}"
              f"{'GRS':>6}{'CRS':>6}  state")
        print("-" * 104)
        for n, f, g, c, base, b, d, st, ok in rows:
            print(f"{n:<38}{f:5.0f}{base:7.1f}{b:7.1f}{d:6.2f}{g:6.0f}{c:6.0f}  "
                  f"{st}{'' if ok else '   <-- OUT OF BAND'}")
        print(f"\nband failures: {fail}    severity ordering monotonic: {mono}")
    return rows, fail, mono


if __name__ == "__main__":
    rows, fail, mono = run_suite()
    print("\nCamera-dead degradation on the worst case:")
    eng = FusionEngine()
    w = np.array([0, 1, 0, 1, 1, 1.0])          # w_cnn -> 0
    for _ in range(120):
        frs, grs, crs, base, b, d, e = eng.step([0.92, 0.82, 0.85, 0.50, 0.65, 0.88], w)
    print(f"  FRS {frs:.0f} (vs 100 with the camera alive) -> "
          f"{state_of(frs, grs, crs, e[0])}")
    raise SystemExit(1 if (fail or not mono) else 0)
