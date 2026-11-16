"""
calibrate_fusion.py — turn the fusion coefficients from informed priors into
measured, defensible values.

  python calibrate_fusion.py --logs "logs/*.csv" --fit
  python calibrate_fusion.py --logs "logs/*.csv" --verify     # replay only

A risk score is defensible when it satisfies stated ordering constraints on
recorded data, with a measured false-alarm rate and a measured detection
latency. Not when it merely looks reasonable.

Procedure:
  1. replay recorded logs offline through fusion_ref.FusionEngine
  2. check the replay reproduces the on-device FRS trace (it must, before you
     tune anything — you cannot calibrate through a discrepancy)
  3. fit the ~15 free coefficients against session-level constraints
  4. re-run the synthetic regression suite so the fit cannot break the ordering
  5. report the four headline numbers

  pip install numpy pandas scipy
"""
from __future__ import annotations
import argparse, glob, sys, json
import numpy as np
import pandas as pd

from fusion_ref import FusionEngine, BOUNDS, DEFAULTS, run_suite, state_of
from feature_ref import sessionise, LABELS

FS = 2.0

# Protocol severity per label, assigned from WHAT YOU DID — never from what the
# device said. Define this before you look at any output.
SEVERITY = {0: 0, 1: 0, 2: 0, 3: 1, 4: 1, 5: 2, 6: 3, 7: 3, 8: 4, 9: 3}

# Which axis the constraint applies to, and the band it must land in.
# A gas leak with no ignition is a maximal GAS emergency and only a moderate
# FIRE risk, so it is scored on the gas axis. That distinction is the point.
CONSTRAINT = {
    0: ("frs", 0, 12),    1: ("frs", 0, 25),    2: ("frs", 0, 30),
    3: ("frs", 0, 45),    4: ("frs", 20, 55),   5: ("frs", 55, 90),
    6: ("grs", 70, 100),  7: ("crs", 65, 100),  8: ("frs", 75, 100),
    9: ("frs", 70, 100),
}

LAMBDA_MISS = 4.0     # a missed hazard costs 4x a false alarm. This is a value
                      # judgement. State it in your report; do not hide it.
LAMBDA_LAT = 0.02
LAMBDA_ORD = 40.0
TARGET_LATENCY_S = 15.0


def load(pattern):
    files = sorted(glob.glob(pattern))
    if not files:
        sys.exit(f"no logs matched {pattern}")
    df = pd.concat([pd.read_csv(f).assign(src=f) for f in files], ignore_index=True)
    df = df[(df["flags"].astype(int) & 2) == 0]           # drop warm-up rows
    df["session"] = sessionise(df)
    return df.reset_index(drop=True)


def replay(df, params):
    """Re-derive FRS from the LOGGED RAW EVIDENCE, session by session.
    Uses the logged conditioned channels' inputs, so this is the same
    computation the firmware did — with different coefficients."""
    out = []
    for sid, g in df.groupby("session"):
        eng = FusionEngine(**params)
        rows = []
        # Reconstruct the raw (pre-conditioning) evidence the firmware fed in.
        # a_* are Model A's outputs; cnn_* are Model B's; Te is recomputed.
        te = np.clip(0.60 * (g["tC"].diff(int(60 * FS)).fillna(0) / 3.0), 0, 1).values
        sv = np.maximum(0.45 * g["cnn_smkL"].values, g["cnn_smkH"].values)
        raw = np.column_stack([g["cnn_flame"].values, g["a_smoke"].values, sv,
                               g["a_co"].values, te, g["a_gas"].values])
        w = g[["w_fv", "w_sg", "w_sv", "w_cg", "w_te", "w_gg"]].values
        for i in range(len(g)):
            frs, grs, crs, base, b, d, e = eng.step(raw[i], w[i], 0.0, 0.0)
            rows.append((frs, grs, crs, e[0]))
        r = np.array(rows)
        out.append(pd.DataFrame({
            "session": sid, "label": g["label"].values,
            "frs": r[:, 0], "grs": r[:, 1], "crs": r[:, 2], "e_fv": r[:, 3],
            "dev_frs": g["frs"].values,          # what the device computed
        }))
    return pd.concat(out, ignore_index=True)


def session_stats(rep):
    rows = []
    for sid, g in rep.groupby("session"):
        lab = int(g["label"].iloc[0])
        axis, lo, hi = CONSTRAINT.get(lab, ("frs", 0, 100))
        v = g[axis].values
        thr = lo if lo > 0 else 25
        hit = np.where(v >= thr)[0]
        rows.append(dict(session=sid, label=lab, name=LABELS.get(lab, "?"),
                         sev=SEVERITY.get(lab, 0), axis=axis, lo=lo, hi=hi,
                         peak=float(v.max()), n=len(g),
                         latency=float(hit[0] / FS) if len(hit) else np.inf,
                         dev_peak=float(g["dev_frs"].max())))
    return pd.DataFrame(rows)


def loss(params, df):
    st = session_stats(replay(df, params))
    L = 0.0
    for _, r in st.iterrows():
        if r.sev <= 1:
            L += max(0.0, r.peak - r.hi) ** 2
        else:
            L += LAMBDA_MISS * max(0.0, r.lo - r.peak) ** 2
            if np.isfinite(r.latency):
                L += LAMBDA_LAT * max(0.0, r.latency - TARGET_LATENCY_S) ** 2
            else:
                L += LAMBDA_MISS * 400.0
    # ordering: peak on the constrained axis should rank with protocol severity
    if st["sev"].nunique() > 2:
        from scipy.stats import kendalltau
        tau = kendalltau(st["peak"], st["sev"]).statistic
        L += LAMBDA_ORD * (1.0 - (0.0 if np.isnan(tau) else tau))
    return L


def report(st, tag):
    print(f"\n=== {tag} ===")
    print(st[["name", "sev", "axis", "peak", "lo", "hi", "latency", "n"]]
          .to_string(index=False, float_format=lambda v: f"{v:7.1f}"))

    benign = st[st.sev <= 1]
    hazard = st[st.sev >= 2]
    severe = st[st.sev >= 3]
    fa = int((benign.peak > 45).sum())
    hours = st.n.sum() / FS / 3600.0
    ben_h = benign.n.sum() / FS / 3600.0
    det = (severe.peak >= severe.lo).mean() if len(severe) else float("nan")
    lat = hazard.latency.replace(np.inf, np.nan).median()
    from scipy.stats import kendalltau
    tau = kendalltau(st.peak, st.sev).statistic if st.sev.nunique() > 1 else np.nan

    print(f"\n  false alarms (>=SMOKE WARNING on benign)  {fa} in "
          f"{ben_h:.1f} benign-hours  ->  {fa/max(ben_h,1e-6):.2f} per hour")
    print(f"  detection rate on severity 3-4            {det*100:.0f}%")
    print(f"  median time-to-alarm on hazards           {lat:.1f} s")
    print(f"  ordering, Kendall tau(peak, severity)     {tau:.3f}")
    print(f"  total recorded                            {hours:.1f} h, "
          f"{len(st)} sessions")
    if hours < 20:
        print("\n  Not enough data to calibrate on. Target >= 20 h across >= 30 sessions,\n"
              "  on >= 3 different days, with >= 8 h of pure benign cooking — the\n"
              "  negative class is what sets your false-alarm rate and it is always\n"
              "  the one people under-collect.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="logs/*.csv")
    ap.add_argument("--fit", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--iters", type=int, default=15)
    ap.add_argument("--popsize", type=int, default=4)
    ap.add_argument("--max-sessions", type=int, default=0,
                    help="cap sessions per label to keep the fit tractable")
    ap.add_argument("--out", default="fitted_coefficients.json")
    a = ap.parse_args()

    df = load(a.logs)
    print(f"{len(df)} rows, {df.session.nunique()} sessions, "
          f"{len(df)/FS/3600:.1f} h")

    rep = replay(df, DEFAULTS)
    err = np.abs(rep.frs - rep.dev_frs)
    print(f"\nreplay vs on-device FRS: mean |err| {err.mean():.2f}, "
          f"p99 {np.percentile(err,99):.2f}")
    if err.mean() > 1.0:
        print("  Replay does not match the device. Fix that before fitting:\n"
              "  check that config.h and fusion_ref.DEFAULTS hold the same values,\n"
              "  and that the Te reconstruction here matches sensor_node.ino.")

    st0 = session_stats(rep)
    report(st0, "current coefficients")
    if a.verify or not a.fit:
        return

    from scipy.optimize import differential_evolution
    import time
    keys = list(BOUNDS.keys())
    bounds = [BOUNDS[k] for k in keys]

    fitdf = df
    if a.max_sessions:
        keep = (df.groupby("label")["session"].transform(
                lambda s: s.isin(sorted(s.unique())[:a.max_sessions])))
        fitdf = df[keep]
        print(f"fitting on {fitdf.session.nunique()} of {df.session.nunique()} sessions")

    def f(x):
        return loss({**DEFAULTS, **dict(zip(keys, x))}, fitdf)

    t0 = time.time(); f([BOUNDS[k][0] for k in keys]); per = time.time() - t0
    n_eval = a.popsize * len(keys) * (a.iters + 1)
    print(f"\nfitting {len(keys)} coefficients: ~{n_eval} evaluations at "
          f"{per:.1f}s each  ->  about {n_eval*per/60:.0f} min.")
    if n_eval * per > 7200:
        print("  That is a long run. Use --max-sessions 3 to fit on a subset, or\n"
              "  --iters 8, then verify the result on the full set with --verify.")
    res = differential_evolution(f, bounds, maxiter=a.iters, popsize=a.popsize,
                                 tol=1e-3, seed=0, polish=True, disp=True)
    fitted = {**DEFAULTS, **dict(zip(keys, res.x))}

    report(session_stats(replay(df, fitted)), "fitted coefficients")

    print("\nre-running the synthetic regression suite with the fitted values:")
    _, fail, mono = run_suite(verbose=True, **{k: fitted[k] for k in keys})
    if fail or not mono:
        print("\nREJECTED: the fit breaks the required ordering or bands.\n"
              "Tighten BOUNDS in fusion_ref.py, or collect more data. Do not ship it.")
        return

    json.dump({k: float(fitted[k]) for k in keys}, open(a.out, "w"), indent=2)
    print(f"\nwrote {a.out}")
    print("\nCopy these into firmware/sensor_node/config.h:")
    name = {"c_Fv": "C_FV", "c_Sg": "C_SG", "c_Sv": "C_SV", "c_Cg": "C_CG",
            "c_Te": "C_TE", "c_Gg": "C_GG", "k_Fv_Gg": "K_FV_GG",
            "k_Fv_Sg": "K_FV_SG", "k_Sv_Sg": "K_SV_SG", "k_Sg_Te": "K_SG_TE",
            "k_Cg_Sg": "K_CG_SG", "k_triple": "K_TRIPLE", "d_cook": "D_COOK",
            "d_steam": "D_STEAM", "persist_rise_s": "PERSIST_RISE_S",
            "persist_floor": "PERSIST_FLOOR"}
    for k in keys:
        print(f"  #define {name[k]:<16} {fitted[k]:.3f}f")


if __name__ == "__main__":
    raise SystemExit("Archived three-sensor trainer: incompatible with two-mq-ads1115-v2. Collect labelled two-sensor data and fit/validate a new model first.")
    main()

