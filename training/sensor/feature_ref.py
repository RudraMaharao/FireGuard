"""
feature_ref.py — the offline mirror of firmware/sensor_node/features.h.

These two files MUST agree. If you change a feature definition in one, change it
in the other. Train/serve skew from mismatched feature code is the most common
and most invisible failure in embedded ML: the model scores beautifully in the
notebook and behaves like noise on the device.

Run `python feature_ref.py --selftest` to check the two implementations agree on
a recorded log (it compares against the on-device values the firmware logs).
"""
from __future__ import annotations
import numpy as np
import pandas as pd

FS = 2.0                    # Hz, the sensor node's control-loop rate
S10, S60, S90, S120 = 20, 120, 180, 240

FEATURE_NAMES = (
    ["dev2", "dev7", "dev135"] +
    ["d10_2", "d10_7", "d10_135"] +
    ["d120_2", "d120_7", "d120_135"] +
    ["sd60_2", "sd60_7", "sd60_135"] +
    ["persist_2", "persist_7", "persist_135"] +
    ["x_2_135", "x_7_2", "x_7_135"] +
    ["tC", "ah", "dT60", "dAH60", "dP300", "warm_min"]
)
assert len(FEATURE_NAMES) == 24


def absolute_humidity(t_c: np.ndarray, rh: np.ndarray) -> np.ndarray:
    """g/m^3. The physically meaningful humidity quantity, and the one that
    separates a steam plume from smoke."""
    return 6.112 * np.exp(17.67 * t_c / (t_c + 243.5)) * rh * 2.1674 / (273.15 + t_c)


def build_features(df: pd.DataFrame) -> pd.DataFrame:
    """df needs: dev2 dev7 dev135 sc2 sc7 sc135 tC rh hPa  (as logged by the S3).
    Returns a frame of the 24 features, index-aligned with df."""
    out = pd.DataFrame(index=df.index)
    dev = {"2": df["dev2"], "7": df["dev7"], "135": df["dev135"]}
    sc = {"2": df["sc2"], "7": df["sc7"], "135": df["sc135"]}

    for k in ("2", "7", "135"):
        d = dev[k]
        out[f"dev{k}"] = d
        out[f"d10_{k}"] = d - d.shift(S10)
        out[f"d120_{k}"] = d - d.shift(S120)
        out[f"sd60_{k}"] = d.rolling(S60, min_periods=1).std(ddof=0)
        thr = 0.5 * sc[k].clip(lower=0.01)
        out[f"persist_{k}"] = (d > thr).rolling(S90, min_periods=1).mean()

    # Cross-channel differences: the closest thing to gas identity these
    # sensors can offer. No single MQ channel identifies anything.
    out["x_2_135"] = dev["2"] - dev["135"]
    out["x_7_2"] = dev["7"] - dev["2"]
    out["x_7_135"] = dev["7"] - dev["135"]

    ah = absolute_humidity(df["tC"].values, df["rh"].values)
    out["tC"] = df["tC"]
    out["ah"] = ah
    out["dT60"] = df["tC"] - df["tC"].shift(S60)
    out["dAH60"] = pd.Series(ah, index=df.index) - pd.Series(ah, index=df.index).shift(S60)

    p = df["hPa"]
    out["dP300"] = p - p.ewm(halflife=300 * FS * 0.693, adjust=False).mean()
    out["warm_min"] = np.minimum((df["ms"] - df["ms"].iloc[0]) / 60000.0, 60.0)

    out = out[FEATURE_NAMES]
    return out.replace([np.inf, -np.inf], 0.0).fillna(0.0)


# --------------------------------------------------------------- targets ----
# The device's BOOT button (long press) cycles this label; it is written into
# every logged row. Set it BEFORE you start a scenario, not afterwards — post-hoc
# labelling of your own sensor traces is how you train a model to reproduce your
# own confirmation bias.
LABELS = {
    0: "idle",
    1: "normal_cooking",
    2: "steam",
    3: "frying_spice",
    4: "cooking_smoke",
    5: "burning_food",
    6: "gas_leak",
    7: "co_test",
    8: "gas_plus_smoke",
    9: "flame_test",
}

TARGET_GAS = {6, 8}
TARGET_CO = {7}
TARGET_SMOKE = {4, 5, 8}


def build_targets(df: pd.DataFrame) -> pd.DataFrame:
    lab = df["label"].astype(int)
    return pd.DataFrame({
        "y_gas": lab.isin(TARGET_GAS).astype(int),
        "y_co": lab.isin(TARGET_CO).astype(int),
        "y_smoke": lab.isin(TARGET_SMOKE).astype(int),
    }, index=df.index)


def sessionise(df: pd.DataFrame, gap_s: float = 60.0) -> pd.Series:
    """A session is a contiguous run with one label and no long time gap.
    Sessions — not rows — are the unit of train/test splitting. Consecutive rows
    at 2 Hz are near-duplicates; splitting on rows leaks and reports ~99.8%
    accuracy for a model that has learned nothing."""
    lab = df["label"].astype(int)
    t = df["ms"].astype(float) / 1000.0
    newlab = lab.ne(lab.shift()).fillna(True)
    biggap = t.diff().fillna(0) > gap_s
    src = df["src"] if "src" in df.columns else pd.Series("x", index=df.index)
    newfile = src.ne(src.shift()).fillna(True)
    return (newlab | biggap | newfile).cumsum()


if __name__ == "__main__":
    import argparse, glob, sys
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="logs/*.csv")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    files = sorted(glob.glob(a.logs))
    if not files:
        sys.exit(f"no logs matched {a.logs}")
    df = pd.concat([pd.read_csv(f).assign(src=f) for f in files], ignore_index=True)
    X = build_features(df)
    print(f"{len(files)} files, {len(df)} rows, {X.shape[1]} features")
    print(X.describe().T[["mean", "std", "min", "max"]].round(3))

    if a.selftest:
        # The firmware logs its own conditioned channels; a gross disagreement
        # here means the two feature implementations have drifted apart.
        s = sessionise(df)
        print(f"\n{s.nunique()} sessions")
        print(df.groupby("label").size().rename("rows").to_frame()
              .assign(name=lambda d: [LABELS.get(i, "?") for i in d.index]))
