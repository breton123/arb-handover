"""Leakage audit of the pool-EV pipeline (`export_bundle.py`), assuming the labeller itself is leakage free.

Rebuilds the bundle pipeline as a baseline, then runs controls that a leak would fail:

  T1 shuffled labels     train on permuted targets (same rows, same features) -> test skill must fall to chance
  T2 single features     test AUC of each of the 173 inputs alone; anything near-perfect is inspected
  T3 train-time universe recompute n_pools_* using only pools seen by the end of train (the offline extras used
                         every partition's market list, test included); rescore the baseline models
  T4 embargo retrain     drop the last 15% of train (by slot) from every fit and table; a large drop means the
                         result leans on proximity to the test window
  T5 time decay          test skill and capture by quarter of the test window (and of validation)
  T6 memorisation        test transactions whose raw feature vector appears verbatim in train; skill without them;
                         capture on positives whose pools were never touched in train

    python experiments/leakage_audit.py --week ... --run ... --credentials tigris.json [--threads 16]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

import numpy as np
import polars as pl
from sklearn.metrics import roc_auc_score

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from nolabel_negative import keep_nolabel, SUB  # noqa: E402
from pool_levers import (BUDGETS, EXTRA_NUM, FEE, PRIOR, ROUTES_PER_PACK, SOL, attach_priors,  # noqa: E402
                         fetch_route_pools, fit, pool_priors, predict)
from export_bundle import route_tables  # noqa: E402

from firm_ml import metrics, splits, week  # noqa: E402

L = 3
RAW = None  # filled with txflat-v2 + extras


def load(W, week_root: Path) -> pl.DataFrame:
    tx = splits.assign_split(week.load_transactions(W), W.bounds)
    side = pl.concat([week._rekey(pl.concat([pl.read_parquet(p.root / "tx_labels" / n, columns=["master_tx_id", "unknown_reasons", "profitable_routes"])
                                              for n in p.manifest["labels"]["parts"]]), W.index(p)).select("master_tx_id", "unknown_reasons", "profitable_routes")
                      for p in W.parts])
    ex = pl.concat([week._rekey(pl.read_parquet(week_root / "extras" / f"{p.root.name}.parquet"), W.index(p))
                    .drop("local_master_tx_id", "partition_index") for p in W.parts])
    tx = tx.join(side, on="master_tx_id", how="left").join(ex, on="master_tx_id", how="left").with_columns(
        pl.when(pl.col("arb_label") == "UNKNOWN").then(pl.col("unknown_reasons")).otherwise(pl.col("arb_label")).alias("group"))
    return tx.with_columns(
        pl.when(pl.col("group") == "POSITIVE").then(1).when(pl.col("group").is_in(["CERTIFIED_NEGATIVE", "NO_LABEL"])).then(0)
        .otherwise(None).cast(pl.Int8).alias("target3"))


def with_extras(tx: pl.DataFrame, universe: set | None = None) -> pl.DataFrame:
    """Raw extras; with `universe`, pool lists are first restricted to pools known at that time."""
    if universe is not None:
        u = pl.Series("u", sorted(universe))
        tx = tx.with_columns([pl.col(c).list.filter(pl.element().is_in(u)) for c in ("pools_all", "pools_static", "pools_writable")])
    return tx.with_columns(
        pl.col("pools_all").list.len().fill_null(0).alias("n_pools_all"), pl.col("pools_static").list.len().fill_null(0).alias("n_pools_static"),
        pl.col("pools_writable").list.len().fill_null(0).alias("n_pools_writable"),
        pl.col("max_venue_swap_amount").cast(pl.Float64).log1p().alias("log_venue_swap"),
        pl.col("max_jup_in_amount").cast(pl.Float64).log1p().alias("log_jup_in"), pl.col("wsol_in_keys").cast(pl.UInt8))


def build(tx: pl.DataFrame, routes: pl.DataFrame, train_end_slot: int):
    """Out-of-fold priors on train rows (slot <= train_end_slot); val/test get all-train tables. Rows between
    train_end_slot and the val window (an embargo) are dropped."""
    tr = tx.filter((pl.col("split") == "train") & (pl.col("slot") <= train_end_slot))
    edges = np.quantile(tr["slot"].to_numpy(), np.linspace(0, 1, 6))
    tr = tr.with_columns(pl.col("slot").cut(list(edges[1:-1]), labels=[str(i) for i in range(5)]).alias("fold"))
    parts = []
    for f in range(5):
        rest = tr.filter(pl.col("fold") != str(f))
        _, n_f = route_tables(rest.filter(pl.col("target3") == 1), routes)
        parts.append(attach_priors(tr.filter(pl.col("fold") == str(f)), pool_priors(tx, rest), n_f))
    pool_routes, pool_n = route_tables(tr.filter(pl.col("target3") == 1), routes)
    stats = pool_priors(tx, tr)
    vt = tx.filter(pl.col("split") != "train")
    out = pl.concat([pl.concat(parts).drop("fold"), attach_priors(vt, stats, pool_n)], how="vertical_relaxed")
    return out, stats, pool_routes, pool_n


def fit_models(tx, cols, kinds, threads, shuffle=False):
    lab = tx.filter(pl.col("target3").is_not_null() & (pl.col("encode_status") == 0) & (pl.col("split") != "test"))
    lab = lab.filter((pl.col("group") != "NO_LABEL") | keep_nolabel(pl.col("master_tx_id"))).with_columns(
        pl.when(pl.col("group") == "NO_LABEL").then(float(SUB)).otherwise(1.0).alias("w"))
    if shuffle:   # permute targets within each split: rows, weights and features unchanged
        lab = pl.concat([d.with_columns(pl.Series("target3", np.random.default_rng(7).permutation(d["target3"].to_numpy())))
                         for d in lab.partition_by("split")])
    cm, ce = fit(lab, cols, kinds, "target3", "w", threads)
    if shuffle:
        return cm, ce, None, None
    pos = tx.filter((pl.col("target3") == 1) & (pl.col("best_net_profit") > 0) & (pl.col("split") != "test")).with_columns(
        pl.col("best_net_profit").cast(pl.Float64).log().alias("logp"))
    sm, se = fit(pos, cols, kinds, "logp", None, threads, "regression")
    return cm, ce, sm, se


def evaluate(vt, cm, ce, sm, se, pool_routes, pool_n, hours, chunks=4) -> dict:
    """Classifier skill, and SOL captured at val-fitted thresholds (budgets), overall and per time chunk."""
    p_arb = predict(cm, ce, vt)
    vt = vt.with_columns(pl.Series("p_arb", p_arb))
    out = {}
    for s in ("val", "test"):
        d = vt.filter((pl.col("split") == s) & pl.col("target3").is_not_null())
        c = metrics.classifier_summary(d["target3"].to_numpy(), d["p_arb"].to_numpy())
        out[f"{s}_pr_auc"], out[f"{s}_roc_auc"], out[f"{s}_base_rate"] = c["pr_auc"], c["roc_auc"], c["base_rate"]
    if sm is None:
        return out
    vt = vt.with_columns((pl.col("p_arb") * pl.Series(np.exp(predict(sm, se, vt)))).alias("ev"))
    pos = vt.filter(pl.col("target3") == 1)
    c = (pos.select("master_tx_id", pl.col("pools_all").alias("pool")).explode("pool").drop_nulls("pool")
         .join(pool_routes, on="pool", how="inner").group_by("master_tx_id", "route").agg(pl.col("support").max())
         .sort(["master_tx_id", "support", "route"], descending=[False, True, False])
         .with_columns(pl.int_range(pl.len()).over("master_tx_id").alias("r")))
    prof = pos.select("master_tx_id", pl.col("profitable_routes").str.split(",").alias("route")).explode("route")
    hit = c.join(prof, on=["master_tx_id", "route"], how="inner").group_by("master_tx_id").agg(pl.col("r").min().alias("hit_rank"))
    cand = (vt.select("master_tx_id", pl.col("pools_all").alias("pool")).explode("pool").drop_nulls("pool")
            .join(pool_n, on="pool", how="inner").select("master_tx_id").unique().with_columns(pl.lit(True).alias("has_candidate")))
    d = (vt.select("master_tx_id", "split", "slot", "target3", "best_net_profit", "encode_status", "ev", "p_arb", "pools_all")
         .join(cand, on="master_tx_id", how="left").join(hit, on="master_tx_id", how="left").with_columns(pl.col("has_candidate").fill_null(False))
         .with_columns(pl.when(pl.col("has_candidate") & (pl.col("encode_status") == 0)).then(pl.col("ev")).otherwise(float("-inf")).alias("_s")))
    sv = np.sort(d.filter(pl.col("split") == "val")["_s"].to_numpy())[::-1]
    for b in BUDGETS:
        n = int(b * hours["val"])
        th = float(sv[n - 1]) if 0 < n <= len(sv) and np.isfinite(sv[n - 1]) else float("inf")
        for s in ("val", "test"):
            x = d.filter(pl.col("split") == s)
            sent = x.filter(pl.col("_s") >= th)
            got = sent.filter((pl.col("target3") == 1) & (pl.col("hit_rank") < ROUTES_PER_PACK * L))
            tot = float(x.filter(pl.col("target3") == 1)["best_net_profit"].sum())
            out[f"{s}_share_{b}"] = float(got["best_net_profit"].sum()) / tot
            out[f"{s}_sol_h_{b}"] = float(got["best_net_profit"].sum()) / SOL / hours[s]
            out[f"{s}_dec_h_{b}"] = sent.height / hours[s]
            if b == 1000:   # time decay: chunks of each window by slot
                q = np.quantile(x["slot"].to_numpy(), np.linspace(0, 1, chunks + 1))
                rows = []
                for i in range(chunks):
                    xi = x.filter(pl.col("slot").is_between(q[i], q[i + 1], closed="left" if i < chunks - 1 else "both"))
                    li = xi.filter(pl.col("target3").is_not_null())
                    gi = xi.filter((pl.col("_s") >= th) & (pl.col("target3") == 1) & (pl.col("hit_rank") < ROUTES_PER_PACK * L))
                    ti = float(xi.filter(pl.col("target3") == 1)["best_net_profit"].sum())
                    rows.append({"chunk": i, "pr_auc": metrics.classifier_summary(li["target3"].to_numpy(), li["p_arb"].to_numpy())["pr_auc"],
                                 "value_share_1000": float(gi["best_net_profit"].sum()) / ti if ti else None,
                                 "positives": int((xi["target3"] == 1).sum())})
                out[f"{s}_chunks_1000"] = rows
    out["_frame"] = d
    return out


def row_hash(m: np.ndarray) -> list[str]:
    m = np.nan_to_num(m, nan=-1.2345e300)
    return [hashlib.blake2b(r.tobytes(), digest_size=12).hexdigest() for r in m]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--run", required=True, type=Path)
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--embargo", type=float, default=0.15)
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE.parent / "weekly"))
    if not (HERE.parent / "weekly" / "fleet").exists():
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, fetch, published

    out_dir = args.run / "experiments"
    W = week.open_week(args.week)
    prep = W.prep
    hours = {t["split"]: t["chain_hours"] for t in json.loads((args.run / "metrics.json").read_text())["splits"]["table"]}
    cols = prep.tx_features + EXTRA_NUM + PRIOR
    raw_cols = prep.tx_features + EXTRA_NUM
    kinds = {**prep.kinds, **{c: "numeric" for c in EXTRA_NUM + PRIOR}}
    res = {}

    base_tx = load(W, args.week)
    routes = fetch_route_pools(args.week, Store(workers=16), fetch, published,
                               [p.root.name for p in W.parts if p.manifest["config"]["min_slot"] <= W.bounds["train"][1]])
    train_end = W.bounds["train"][1]
    print("loaded", base_tx.height, flush=True)

    # Baseline = the bundle pipeline.
    tx, stats, pool_routes, pool_n = build(with_extras(base_tx), routes, train_end)
    cm, ce, sm, se = fit_models(tx, cols, kinds, args.threads)
    vt = tx.filter(pl.col("split") != "train")
    base = evaluate(vt, cm, ce, sm, se, pool_routes, pool_n, hours)
    frame = base.pop("_frame")
    res["baseline"] = base
    print("baseline", {k: v for k, v in base.items() if not k.endswith("chunks_1000")}, flush=True)

    # T2 single-feature AUC on test (labelled + NO_LABEL rows).
    te = vt.filter((pl.col("split") == "test") & pl.col("target3").is_not_null() & (pl.col("encode_status") == 0))
    X, y = ce.transform(te), te["target3"].to_numpy()
    aucs = []
    for j, c in enumerate(ce.columns):
        v = np.nan_to_num(X[:, j], nan=-1e300)
        if np.unique(v).size < 2:
            continue
        a = roc_auc_score(y, v)
        aucs.append({"feature": c, "auc": float(max(a, 1 - a)), "direction": "+" if a >= 0.5 else "-"})
    res["single_feature_auc_test"] = sorted(aucs, key=lambda r: -r["auc"])[:25]
    print("top single-feature AUC", res["single_feature_auc_test"][:8], flush=True)

    # T6 memorisation: raw feature vectors of test rows seen verbatim in train.
    from firm_ml import encoding
    renc = encoding.FeatureEncoder(raw_cols, kinds).fit(tx.filter(pl.col("split") == "train"))
    tr_lab = tx.filter((pl.col("split") == "train") & pl.col("target3").is_not_null())
    tr_h = set(row_hash(renc.transform(tr_lab)))
    tr_pos_h = set(row_hash(renc.transform(tr_lab.filter(pl.col("target3") == 1))))
    te_h = row_hash(renc.transform(te))
    seen = np.array([h in tr_h for h in te_h])
    seen_pos = np.array([h in tr_pos_h for h in te_h])
    p_te = predict(cm, ce, te)
    unseen = metrics.classifier_summary(y[~seen], p_te[~seen])
    pools_train = set(stats["pool"].to_list())
    tpos = frame.filter((pl.col("split") == "test") & (pl.col("target3") == 1)).with_columns(
        pl.col("pools_all").list.eval(pl.element().is_in(pl.Series(sorted(pools_train)))).list.any().fill_null(False).alias("known_pool"))
    res["memorisation"] = {
        "test_rows_with_train_duplicate": float(seen.mean()), "test_positives_with_train_duplicate": float(seen[y == 1].mean()),
        "test_positives_duplicating_a_train_positive": float(seen_pos[y == 1].mean()),
        "test_pr_auc_excluding_duplicates": unseen["pr_auc"], "base_rate_excluding_duplicates": unseen["base_rate"],
        "test_positives_touching_no_train_pool": float((~tpos["known_pool"]).mean()),
        "value_share_of_those": float(tpos.filter(~pl.col("known_pool"))["best_net_profit"].sum()) / float(tpos["best_net_profit"].sum()),
    }
    print("memorisation", res["memorisation"], flush=True)

    # T3 train-time pool universe: only pools touched by train transactions count in n_pools_*.
    universe = set(base_tx.filter(pl.col("split") == "train").select(pl.col("pools_all").explode().drop_nulls().unique())["pools_all"].to_list())
    universe |= set(pool_routes["pool"].unique().to_list())   # pools named by train routes are known at train time too
    vt_u = attach_priors(with_extras(base_tx.filter(pl.col("split") != "train"), universe), stats, pool_n)
    u = evaluate(vt_u, cm, ce, sm, se, pool_routes, pool_n, hours)
    u.pop("_frame")
    res["train_time_universe"] = {"pools_in_universe": len(universe), **u}
    print("train-time universe", {k: v for k, v in u.items() if not k.endswith("chunks_1000")}, flush=True)

    # T1 shuffled labels.
    sc, se_c, _, _ = fit_models(tx, cols, kinds, args.threads, shuffle=True)
    res["shuffled_labels"] = evaluate(vt, sc, se_c, None, None, pool_routes, pool_n, hours)
    print("shuffled", res["shuffled_labels"], flush=True)

    # T4 embargo: fit everything on train minus its last `embargo` share of slots.
    trs = np.sort(base_tx.filter(pl.col("split") == "train")["slot"].unique().to_numpy())
    cut = int(trs[int(len(trs) * (1 - args.embargo)) - 1])
    tx_e, stats_e, routes_e, n_e = build(with_extras(base_tx), routes, cut)
    tx_e = tx_e.filter((pl.col("split") != "train") | (pl.col("slot") <= cut))
    cm_e, ce_e, sm_e, se_e = fit_models(tx_e, cols, kinds, args.threads)
    e = evaluate(tx_e.filter(pl.col("split") != "train"), cm_e, ce_e, sm_e, se_e, routes_e, n_e, hours)
    e.pop("_frame")
    res["embargo"] = {"train_end_slot": cut, "embargo_share": args.embargo, **e}
    print("embargo", {k: v for k, v in e.items() if not k.endswith("chunks_1000")}, flush=True)

    (out_dir / "leakage_audit.json").write_text(json.dumps(res, indent=1, default=str))
    print("done")


if __name__ == "__main__":
    main()
