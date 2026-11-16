"""
train_sensor_model.py — Model A: gas / environmental intelligence.

  python train_sensor_model.py --logs "logs/*.csv" --out ../firmware/sensor_node/model_a_trained.h

Three independent binary gradient-boosted ensembles (gas / CO / smoke), exported
to plain C as flat tree arrays. Not a neural network, deliberately:
  * 24 tabular features is exactly where GBMs beat small MLPs
  * the export has no tensor arena and no runtime dependency
  * you can read feature importances, which matters at 2 a.m. when it false-alarms

  pip install pandas numpy scikit-learn

Heads are INDEPENDENT SIGMOIDS, not a softmax: gas leakage and smoke genuinely
co-occur, and making them compete for probability mass would destroy exactly the
signal the fusion layer needs.
"""
from __future__ import annotations
import argparse, glob, sys, textwrap
import numpy as np
import pandas as pd
from sklearn.ensemble import GradientBoostingClassifier
from sklearn.model_selection import GroupKFold
from sklearn.metrics import average_precision_score, precision_recall_curve

from feature_ref import build_features, build_targets, sessionise, FEATURE_NAMES, LABELS

HEADS = ["gas", "co", "smoke"]
FS = 2.0


def load(pattern: str) -> pd.DataFrame:
    files = sorted(glob.glob(pattern))
    if not files:
        sys.exit(f"no logs matched {pattern}")
    df = pd.concat([pd.read_csv(f).assign(src=f) for f in files], ignore_index=True)
    # The first WARMUP window is not representative of anything.
    if "flags" in df.columns:
        df = df[(df["flags"].astype(int) & 2) == 0]
    return df.reset_index(drop=True)


def fit_head(X, y, groups, name, n_est, depth, seed):
    """Grouped CV first (the honest number), then refit on everything."""
    n_groups = len(np.unique(groups))
    n_splits = min(5, max(2, n_groups))
    gkf = GroupKFold(n_splits=n_splits)
    aps, base = [], y.mean()

    for tr, te in gkf.split(X, y, groups):
        if y[tr].sum() == 0 or y[te].sum() == 0:
            continue
        m = GradientBoostingClassifier(n_estimators=n_est, max_depth=depth,
                                       learning_rate=0.08, subsample=0.85,
                                       min_samples_leaf=max(10, int(5 * FS)),
                                       random_state=seed)
        m.fit(X[tr], y[tr])
        aps.append(average_precision_score(y[te], m.predict_proba(X[te])[:, 1]))

    ap = float(np.mean(aps)) if aps else float("nan")
    print(f"  {name:<6} PR-AUC {ap:.3f}  (chance {base:.3f}, {len(aps)} folds, "
          f"{int(y.sum())} positive rows)")
    if aps and ap < base * 1.5:
        print(f"    WARNING: barely above chance. Collect more {name} sessions, "
              f"or check the label mapping in feature_ref.py.")

    final = GradientBoostingClassifier(n_estimators=n_est, max_depth=depth,
                                       learning_rate=0.08, subsample=0.85,
                                       min_samples_leaf=max(10, int(5 * FS)),
                                       random_state=seed)
    final.fit(X, y)
    return final, ap


def report_importance(model, name, k=8):
    imp = sorted(zip(FEATURE_NAMES, model.feature_importances_),
                 key=lambda t: -t[1])[:k]
    print(f"  {name:<6} top features: " +
          ", ".join(f"{n}={v:.2f}" for n, v in imp))


def detection_latency(df, X, models, groups):
    """Median seconds from session start to the head first crossing 0.5.
    This is the metric that actually matters and the one most projects forget."""
    print("\nTime-to-detection (median over hazard sessions):")
    for head, tgt in zip(HEADS, ["y_gas", "y_co", "y_smoke"]):
        p = models[head].predict_proba(X)[:, 1]
        lat = []
        for g in np.unique(groups):
            m = groups == g
            if df[tgt].values[m].max() == 0:
                continue
            hits = np.where(p[m] > 0.5)[0]
            lat.append(hits[0] / FS if len(hits) else np.inf)
        if lat:
            fin = [x for x in lat if np.isfinite(x)]
            print(f"  {head:<6} {np.median(lat) if fin else float('inf'):>6.1f} s   "
                  f"detected {len(fin)}/{len(lat)} sessions")


def flatten_trees(model, Xsample):
    """Emit the ensemble as FLAT ARRAYS rather than nested if/else.

    m2cgen's nested-code output for 80 trees is enormous and sklearn's
    GradientBoostingClassifier is not one of its supported models anyway. Arrays
    plus a ten-line interpreter give the same result in a few KB of .rodata,
    with no code-size blow-up and no external dependency.

    A binary GBC's decision function is  F = F0 + lr * sum(tree_i(x)),  and the
    probability is sigmoid(F). F0 is recovered empirically so we do not depend
    on sklearn's private init API, which has moved between versions.
    """
    nodes, roots = [], []
    for stage in model.estimators_:
        t = stage[0].tree_
        base = len(nodes)
        roots.append(base)
        for i in range(t.node_count):
            if t.children_left[i] == -1:                     # leaf
                nodes.append((-1, float(t.value[i][0][0]), 0, 0))
            else:
                nodes.append((int(t.feature[i]), float(t.threshold[i]),
                              base + int(t.children_left[i]),
                              base + int(t.children_right[i])))
    lr = float(model.learning_rate)
    tree_sum = sum(s[0].predict(Xsample[:1])[0] for s in model.estimators_)
    f0 = float(model.decision_function(Xsample[:1])[0] - lr * tree_sum)
    return nodes, roots, lr, f0


RUNTIME = """
/* --- shared tree interpreter: ~10 lines, float-only (the ESP32 FPU is
   single-precision; doubles are software-emulated and roughly 40x slower) --- */
typedef struct { int16_t feat; float thr; int16_t l, r; } MaNode;

static float ma_eval(const MaNode *nodes, const int16_t *roots, int nTrees,
                     float lr, float f0, const float *x) {
  float F = f0;
  for (int t = 0; t < nTrees; t++) {
    int i = roots[t];
    while (nodes[i].feat >= 0) i = (x[nodes[i].feat] <= nodes[i].thr) ? nodes[i].l : nodes[i].r;
    F += lr * nodes[i].thr;
  }
  return 1.0f / (1.0f + expf(-F));
}
"""


def emit_c(models, out_path, aps, Xsample):
    import os
    parts = [textwrap.dedent(f"""\
        /* ====================================================================
           model_a_trained.h — GENERATED by training/train_sensor_model.py.
           Do not edit by hand.

           Grouped-CV PR-AUC:  gas {aps['gas']:.3f}   co {aps['co']:.3f}   smoke {aps['smoke']:.3f}

           The feature order here must match firmware/sensor_node/features.h
           exactly. If you change one, regenerate this.
           ==================================================================== */
        #pragma once
        #include <stdint.h>
        #include <math.h>
        """), RUNTIME]

    total = 0
    for h in HEADS:
        nodes, roots, lr, f0 = flatten_trees(models[h], Xsample)
        total += len(nodes)
        parts.append(f"\n/* ---------------- {h}: {len(roots)} trees, "
                     f"{len(nodes)} nodes ---------------- */\n")
        parts.append(f"static const MaNode ma_{h}_n[] = {{\n")
        for f, v, l, r in nodes:
            parts.append(f"  {{{f},{v:.6g}f,{l},{r}}},\n")
        parts.append("};\n")
        parts.append(f"static const int16_t ma_{h}_r[] = {{"
                     + ",".join(str(v) for v in roots) + "};\n")
        parts.append(f"static inline void ma_{h}(const float *x, float *out) "
                     f"{{ out[0] = ma_eval(ma_{h}_n, ma_{h}_r, {len(roots)}, "
                     f"{lr:.6g}f, {f0:.6g}f, x); }}\n")

    open(out_path, "w").write("".join(parts))
    kb = os.path.getsize(out_path) / 1024
    print(f"\nwrote {out_path}  ({kb:.0f} KB source, {total} nodes, "
          f"~{total * 12 / 1024:.0f} KB of flash)")
    print("Now set USE_TRAINED_MODEL_A to 1 in firmware/sensor_node/model_a.h and reflash.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="logs/*.csv")
    ap.add_argument("--out", default="../firmware/sensor_node/model_a_trained.h")
    ap.add_argument("--trees", type=int, default=60)
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()

    df = load(a.logs)
    X = build_features(df)
    Y = build_targets(df)
    groups = sessionise(df).values

    print(f"{len(df)} rows, {len(np.unique(groups))} sessions, "
          f"{df['ms'].diff().clip(lower=0).sum()/3.6e6:.1f} hours")
    counts = df.groupby("label").size()
    for lab, n in counts.items():
        print(f"  label {lab:>2} {LABELS.get(int(lab),'?'):<16} {n/FS/60:6.1f} min")

    if len(np.unique(groups)) < 8:
        print("\nWARNING: fewer than 8 sessions. Grouped CV will be unstable and the\n"
              "model will not generalise. Target >= 30 sessions over >= 3 days.")

    Xv = X.values.astype(np.float64)
    print("\nGrouped (session-wise) cross-validation:")
    models, aps = {}, {}
    for h, col in zip(HEADS, ["y_gas", "y_co", "y_smoke"]):
        y = Y[col].values
        if y.sum() < 50:
            print(f"  {h:<6} SKIPPED — only {int(y.sum())} positive rows. "
                  f"Record label sessions for it before training.")
            aps[h] = float("nan")
            models[h] = GradientBoostingClassifier(n_estimators=2, max_depth=1).fit(
                Xv[:20], np.r_[np.zeros(10, int), np.ones(10, int)])
            continue
        models[h], aps[h] = fit_head(Xv, y, groups, h, a.trees, a.depth, a.seed)
        report_importance(models[h], h)

    detection_latency(Y, Xv, models, groups)
    emit_c(models, a.out, aps, Xv)


if __name__ == "__main__":
    raise SystemExit("Archived three-sensor trainer: incompatible with two-mq-ads1115-v2. Collect labelled two-sensor data and fit/validate a new model first.")
    main()

