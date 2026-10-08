"""Landing-head test: how predictable is P(landed_ok) from pre-execution information?

Input: `<week>/population/<partition>.parquet` from `extract_population.py` (landed and failed transactions naming a
labelled-venue pool). Adds causal fee-payer reputation, then trains LightGBM on a 70/15/15 slot split.

Reputation (same idea as the live trader's): per payer, landed and failed counts, halved every HALF_LIFE slots
(~1 hour), counting only transactions at least LAG slots older than the current one (live truth arrives at
`confirmed`, a slot or two late). Nothing from the transaction's own execution is a feature.

Compared: base rate; the partner's "bad payer" rule (>= 20 prior txs, >= 90% failed); reputation only; all features
without reputation; all features.

    python experiments/landing_head.py --week ... --partitions ID ... --out report.json
"""

from __future__ import annotations

import argparse
import json
from collections import deque
from pathlib import Path

import lightgbm as lgb
import numpy as np
import polars as pl
from sklearn.metrics import average_precision_score, brier_score_loss, roc_auc_score

from firm_ml import encoding, models

HALF_LIFE = 9000      # slots, ~1 hour
LAG = 2               # slots
HASH = ["prog0", "prog1", "prog2", "prog_last", "prog_seq"]
NUM = ["version", "uses_alt", "n_alt_tables", "n_static_keys", "n_keys", "n_signers", "n_writable", "n_instructions",
       "n_distinct_programs", "data_bytes", "max_ix_data", "raw_len", "n_compute_budget_ix", "cu_limit", "cu_price",
       "priority_fee", "jito_tip", "has_tip", "n_transfers", "durable_nonce", "payer_only_signer", "n_pools",
       "n_pools_static", "n_pools_writable", "n_venue_swaps", "max_venue_swap", "n_jup_routes", "max_jup_in",
       "wsol_in_keys", "attempted_arb_static", "entry_index", "tx_index"]
REP = ["rep_landed", "rep_failed", "rep_total", "rep_fail_rate", "rep_seen"]


def reputation(df: pl.DataFrame) -> pl.DataFrame:
    """Causal decayed payer counts. `df` sorted by slot, entry, tx."""
    payer = df["fee_payer"].to_list()
    slot = df["slot"].to_numpy()
    ok = df["landed_ok"].to_numpy()
    n = len(payer)
    out_l, out_f = np.zeros(n), np.zeros(n)
    seen = np.zeros(n, dtype=np.int8)
    state: dict[str, list] = {}           # payer -> [landed, failed, epoch]
    pending: deque = deque()              # (slot, payer, ok) not yet visible
    for i in range(n):
        s = slot[i]
        while pending and pending[0][0] <= s - LAG - 1:
            ps, pp, pk = pending.popleft()
            e = ps // HALF_LIFE
            st = state.get(pp)
            if st is None:
                st = state[pp] = [0.0, 0.0, e]
            if e > st[2]:
                f = 0.5 ** (e - st[2]); st[0] *= f; st[1] *= f; st[2] = e
            st[0 if pk else 1] += 1.0
        st = state.get(payer[i])
        if st is not None:
            f = 0.5 ** max(0, s // HALF_LIFE - st[2])
            out_l[i], out_f[i], seen[i] = st[0] * f, st[1] * f, 1
        pending.append((s, payer[i], bool(ok[i])))
    tot = out_l + out_f
    return df.with_columns(pl.Series("rep_landed", out_l), pl.Series("rep_failed", out_f), pl.Series("rep_total", tot),
                           pl.Series("rep_fail_rate", np.where(tot > 0, out_f / np.maximum(tot, 1e-9), np.nan)),
                           pl.Series("rep_seen", seen))


def fit_eval(df, cols, kinds, threads):
    tr, va, te = (df.filter(pl.col("split") == s) for s in ("train", "val", "test"))
    enc = encoding.FeatureEncoder(cols, kinds).fit(tr)
    cat = [enc.columns.index(c) for c in enc.categorical]
    params = {**models.ARB_PARAMS, "num_threads": threads, "metric": ["binary_logloss", "auc"], "num_leaves": 63, "min_data_in_leaf": 200}
    dtr = lgb.Dataset(enc.transform(tr), tr["y"].to_numpy(), feature_name=enc.columns, categorical_feature=cat, free_raw_data=False)
    dva = lgb.Dataset(enc.transform(va), va["y"].to_numpy(), reference=dtr, categorical_feature=cat, free_raw_data=False)
    m = lgb.train(params, dtr, num_boost_round=3000, valid_sets=[dva], valid_names=["val"],
                  callbacks=[lgb.early_stopping(100, first_metric_only=True, verbose=False)])
    p = m.predict(enc.transform(te), num_iteration=m.best_iteration)
    return m, p


def summary(y, p):
    """y = landed_ok. Reports landing AUC, failure PR-AUC (failures are the class to catch), Brier."""
    return {"n": int(len(y)), "landed_rate": float(y.mean()), "roc_auc": float(roc_auc_score(y, p)),
            "fail_pr_auc": float(average_precision_score(1 - y, 1 - p)), "fail_base_rate": float(1 - y.mean()),
            "brier": float(brier_score_loss(y, p))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--partitions", nargs="+", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--threads", type=int, default=16)
    args = ap.parse_args()

    df = pl.concat([pl.read_parquet(args.week / "population" / f"{p}.parquet") for p in args.partitions], how="vertical_relaxed")
    df = df.sort("slot", "entry_index", "tx_index").with_columns(pl.col("landed_ok").cast(pl.Int8).alias("y"))
    print("rows", df.height, "landed", round(float(df["y"].mean()), 4), flush=True)
    df = reputation(df)
    slots = np.sort(df["slot"].unique().to_numpy())
    c1, c2 = slots[int(len(slots) * 0.70)], slots[int(len(slots) * 0.85)]
    df = df.with_columns(pl.when(pl.col("slot") < c1).then(pl.lit("train")).when(pl.col("slot") < c2).then(pl.lit("val"))
                         .otherwise(pl.lit("test")).alias("split"))
    te = df.filter(pl.col("split") == "test")
    y = te["y"].to_numpy()
    kinds = {**{c: "hash" for c in HASH}, **{c: "numeric" for c in NUM + REP}}
    res = {"rows": {s: int(n) for s, n in df.group_by("split").len().iter_rows()}, "partitions": args.partitions,
           "error_kinds_test": dict(te.filter(pl.col("y") == 0).group_by("error_kind").len().sort("len", descending=True).head(8).iter_rows())}

    # Partner's gate: bad payer = >= 20 prior (decayed) txs and >= 90% failed.
    bad = ((te["rep_total"].to_numpy() >= 20) & (np.nan_to_num(te["rep_fail_rate"].to_numpy()) >= 0.9))
    res["bad_payer_rule"] = {"share_flagged": float(bad.mean()), "landed_if_flagged": float(y[bad].mean()) if bad.any() else None,
                             "landed_if_not": float(y[~bad].mean()), "failures_caught": float(((1 - y) * bad).sum() / max(1, (1 - y).sum())),
                             "landed_lost": float((y * bad).sum() / max(1, y.sum()))}
    res["models"] = {}
    for name, cols in {"reputation_only": REP, "no_reputation": HASH + NUM, "all": HASH + NUM + REP}.items():
        m, p = fit_eval(df, cols, kinds, args.threads)
        r = summary(y, p)
        for sub, mask in {"attempted_arb": te["attempted_arb_static"].to_numpy() == 1, "bad_payers": bad,
                          "has_venue_swap": te["n_venue_swaps"].to_numpy() > 0, "jupiter": te["n_jup_routes"].to_numpy() > 0}.items():
            if mask.sum() > 100 and 0 < y[mask].mean() < 1:
                r[f"subset_{sub}"] = summary(y[mask], p[mask])
        # Gate trade-off: skip transactions below each P(land) cut-off.
        r["gate"] = [{"cutoff": c, "kept": float((p >= c).mean()), "landed_rate_kept": float(y[p >= c].mean()) if (p >= c).any() else None,
                      "failures_removed": float(((1 - y) * (p < c)).sum() / max(1, (1 - y).sum())),
                      "landed_lost": float((y * (p < c)).sum() / max(1, y.sum()))} for c in (0.1, 0.2, 0.3, 0.5, 0.7, 0.9)]
        r["calibration"] = [{"bin": f"{lo:.1f}-{lo + 0.1:.1f}", "n": int(((p >= lo) & (p < lo + 0.1)).sum()),
                             "landed": float(y[(p >= lo) & (p < lo + 0.1)].mean()) if ((p >= lo) & (p < lo + 0.1)).any() else None}
                            for lo in np.arange(0, 1, 0.1)]
        r["best_iteration"] = m.best_iteration
        r["importance"] = models.importance(m)[:15]
        res["models"][name] = r
        print(name, {k: v for k, v in r.items() if k in ("roc_auc", "fail_pr_auc", "fail_base_rate", "brier")}, flush=True)
    args.out.write_text(json.dumps(res, indent=1, default=str))
    print("done")


if __name__ == "__main__":
    main()
