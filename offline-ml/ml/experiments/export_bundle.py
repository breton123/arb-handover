"""Export the paper-trading model bundle (pool lookup + value-weighted EV policy, 3-venue labels).

This is the `new` configuration of `pool_levers.py`, retrained and saved, with one fix: every train-row
pool feature (priors AND route counts) is out-of-fold, so no train row sees its own label.

Bundle (`--out`):
    manifest.json            versions, windows, feature order, sources, metrics, thresholds
    arb_classifier.{txt,json,encoder.json}   LightGBM P(arb) (POSITIVE=1; CERTIFIED_NEGATIVE, NO_LABEL=0)
    size_model.{txt,json,encoder.json}       LightGBM E[log net profit lamports | arb]
    pool_stats.parquet       per pool: touch, pos, pos_rate, logprofit, n_routes  (train window only)
    pool_routes.parquet      per pool: top routes by train support (rank 0..191)
    routes.parquet           route_id -> market_ids (pools, in order), hops
    thresholds.json          per budget (decisions / chain hour): EV threshold and live top-L, fitted on val
    eval.json                test results at those thresholds (SOL captured, net, send mix)

    python experiments/export_bundle.py --week ... --run ... --credentials tigris.json --out .../bundle
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import polars as pl

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from nolabel_negative import keep_nolabel, SUB  # noqa: E402
from pool_levers import (BUDGETS, EXTRA_NUM, FEE, LIVE_TOP, PRIOR, ROUTES_PER_PACK, DEPTH_ROUTES, SOL,  # noqa: E402
                         fetch_route_pools, fit, predict, pool_priors, attach_priors)

from firm_ml import metrics, models, splits, week  # noqa: E402

BUNDLE_SCHEMA = "firm-pool-ev-bundle-v1"


def route_tables(pos_rows: pl.DataFrame, routes: pl.DataFrame):
    """Route support from these POSITIVE rows -> (pool_routes capped to max depth, routes-per-pool counts)."""
    sup = (pos_rows.select("master_tx_id", pl.col("profitable_routes").str.split(",")).explode("profitable_routes")
           .filter(pl.col("profitable_routes") != "").group_by("profitable_routes")
           .agg(pl.col("master_tx_id").n_unique().alias("support")).rename({"profitable_routes": "route"}))
    pr = (sup.join(routes.rename({"route_id": "route"}), on="route", how="inner")
          .with_columns(pl.col("market_ids").str.split(",").alias("pool")).explode("pool").select("pool", "route", "support"))
    n = pr.group_by("pool").agg(pl.len().alias("n_routes"))
    pr = (pr.sort(["pool", "support", "route"], descending=[False, True, False])
          .with_columns(pl.int_range(pl.len()).over("pool").alias("rank")).filter(pl.col("rank") < max(DEPTH_ROUTES)))
    return pr, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--run", required=True, type=Path)
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--threads", type=int, default=16)
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE.parent / "weekly"))
    if not (HERE.parent / "weekly" / "fleet").exists():
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, fetch, published

    args.out.mkdir(parents=True, exist_ok=True)
    W = week.open_week(args.week)
    prep = W.prep
    hours = {t["split"]: t["chain_hours"] for t in json.loads((args.run / "metrics.json").read_text())["splits"]["table"]}

    tx = splits.assign_split(week.load_transactions(W), W.bounds)
    side = pl.concat([week._rekey(pl.concat([pl.read_parquet(p.root / "tx_labels" / n, columns=["master_tx_id", "unknown_reasons", "profitable_routes"])
                                              for n in p.manifest["labels"]["parts"]]), W.index(p)).select("master_tx_id", "unknown_reasons", "profitable_routes")
                      for p in W.parts])
    ex = pl.concat([week._rekey(pl.read_parquet(args.week / "extras" / f"{p.root.name}.parquet"), W.index(p))
                    .drop("local_master_tx_id", "partition_index") for p in W.parts])
    tx = tx.join(side, on="master_tx_id", how="left").join(ex, on="master_tx_id", how="left").with_columns(
        pl.when(pl.col("arb_label") == "UNKNOWN").then(pl.col("unknown_reasons")).otherwise(pl.col("arb_label")).alias("group"))
    tx = tx.with_columns(
        pl.when(pl.col("group") == "POSITIVE").then(1).when(pl.col("group").is_in(["CERTIFIED_NEGATIVE", "NO_LABEL"])).then(0)
        .otherwise(None).cast(pl.Int8).alias("target3"),
        pl.col("pools_all").list.len().fill_null(0).alias("n_pools_all"), pl.col("pools_static").list.len().fill_null(0).alias("n_pools_static"),
        pl.col("pools_writable").list.len().fill_null(0).alias("n_pools_writable"),
        pl.col("max_venue_swap_amount").cast(pl.Float64).log1p().alias("log_venue_swap"),
        pl.col("max_jup_in_amount").cast(pl.Float64).log1p().alias("log_jup_in"), pl.col("wsol_in_keys").cast(pl.UInt8))
    print("loaded", tx.height, flush=True)

    store = Store(workers=16)
    train_parts = [p.root.name for p in W.parts if p.manifest["config"]["min_slot"] <= W.bounds["train"][1]]
    routes = fetch_route_pools(args.week, store, fetch, published, train_parts)

    # Train rows: priors and route counts from the other 4 slot folds. Val/test and the bundle: all train.
    tr = tx.filter(pl.col("split") == "train")
    edges = np.quantile(tr["slot"].to_numpy(), np.linspace(0, 1, 6))
    tr = tr.with_columns(pl.col("slot").cut(list(edges[1:-1]), labels=[str(i) for i in range(5)]).alias("fold"))
    parts = []
    for f in range(5):
        rest = tr.filter(pl.col("fold") != str(f))
        _, n_f = route_tables(rest.filter(pl.col("target3") == 1), routes)
        parts.append(attach_priors(tr.filter(pl.col("fold") == str(f)), pool_priors(tx, rest), n_f))
        print("fold", f, flush=True)
    pool_routes, pool_n = route_tables(tr.filter(pl.col("target3") == 1), routes)
    stats = pool_priors(tx, tr).join(pool_n, on="pool", how="full", coalesce=True).with_columns(
        pl.col("touch", "pos", "n_routes").fill_null(0))
    vt = tx.filter(pl.col("split") != "train")
    tx = pl.concat([pl.concat(parts).drop("fold"), attach_priors(vt, stats.select("pool", "touch", "pos", "logprofit", "pos_rate"), pool_n)],
                   how="vertical_relaxed")
    del tr, parts

    cols = prep.tx_features + EXTRA_NUM + PRIOR
    kinds = {**prep.kinds, **{c: "numeric" for c in EXTRA_NUM + PRIOR}}
    lab = tx.filter(pl.col("target3").is_not_null() & (pl.col("encode_status") == 0))
    lab = lab.filter((pl.col("group") != "NO_LABEL") | keep_nolabel(pl.col("master_tx_id"))).with_columns(
        pl.when(pl.col("group") == "NO_LABEL").then(float(SUB)).otherwise(1.0).alias("w"))
    pos = tx.filter((pl.col("target3") == 1) & (pl.col("best_net_profit") > 0)).with_columns(
        pl.col("best_net_profit").cast(pl.Float64).log().alias("logp"))
    cm, ce = fit(lab, cols, kinds, "target3", "w", args.threads)
    sm, se = fit(pos, cols, kinds, "logp", None, args.threads, "regression")
    ev_tx = tx.filter(pl.col("split") != "train")
    p_arb, p_size = predict(cm, ce, ev_tx), predict(sm, se, ev_tx)
    ev_tx = ev_tx.with_columns(pl.Series("p_arb", p_arb), pl.Series("ev", p_arb * np.exp(p_size)))

    quality = {}
    for s in ("val", "test"):
        d = ev_tx.filter((pl.col("split") == s) & pl.col("target3").is_not_null())
        c = metrics.classifier_summary(d["target3"].to_numpy(), d["p_arb"].to_numpy())
        ps = pos.filter(pl.col("split") == s)
        sp, a = predict(sm, se, ps), ps["logp"].to_numpy()
        rk = lambda v: pl.Series(v).rank().to_numpy()
        quality[s] = {"arb_pr_auc": c["pr_auc"], "arb_roc_auc": c["roc_auc"], "base_rate": c["base_rate"],
                      "size_spearman": float(np.corrcoef(rk(sp), rk(a))[0, 1])}
    print("quality", json.dumps(quality), flush=True)

    # Candidate hit ranks for val/test positives, candidate existence for all val/test txs.
    pos_vt = ev_tx.filter(pl.col("target3") == 1)
    c = (pos_vt.select("master_tx_id", pl.col("pools_all").alias("pool")).explode("pool").drop_nulls("pool")
         .join(pool_routes, on="pool", how="inner").group_by("master_tx_id", "route").agg(pl.col("support").max())
         .sort(["master_tx_id", "support", "route"], descending=[False, True, False])
         .with_columns(pl.int_range(pl.len()).over("master_tx_id").alias("r")))
    prof = pos_vt.select("master_tx_id", pl.col("profitable_routes").str.split(",").alias("route")).explode("route")
    hit = c.join(prof, on=["master_tx_id", "route"], how="inner").group_by("master_tx_id").agg(pl.col("r").min().alias("hit_rank"))
    cand = (ev_tx.select("master_tx_id", pl.col("pools_all").alias("pool")).explode("pool").drop_nulls("pool")
            .join(pool_n, on="pool", how="inner").select("master_tx_id").unique().with_columns(pl.lit(True).alias("has_candidate")))
    d = (ev_tx.select("master_tx_id", "split", "target3", "group", "best_net_profit", "encode_status", "ev")
         .join(cand, on="master_tx_id", how="left").join(hit, on="master_tx_id", how="left").with_columns(pl.col("has_candidate").fill_null(False))
         .with_columns(pl.when(pl.col("has_candidate") & (pl.col("encode_status") == 0)).then(pl.col("ev")).otherwise(float("-inf")).alias("_s")))
    sv = np.sort(d.filter(pl.col("split") == "val")["_s"].to_numpy())[::-1]
    grid = []
    for L in LIVE_TOP:
        for budget in BUDGETS:
            n = int(budget * hours["val"])
            th = float(sv[n - 1]) if 0 < n <= len(sv) and np.isfinite(sv[n - 1]) else float("inf")
            row = {"L": L, "budget_per_hour": budget, "ev_threshold": th}
            for s in ("val", "test"):
                x = d.filter(pl.col("split") == s)
                sent = x.filter(pl.col("_s") >= th)
                got = sent.filter((pl.col("target3") == 1) & (pl.col("hit_rank") < ROUTES_PER_PACK * L))
                sol = float(got["best_net_profit"].sum()) / SOL
                mix = dict(sent.group_by("group").len().iter_rows())
                row.update({f"{s}_decisions_per_hour": sent.height / hours[s], f"{s}_captured": got.height,
                            f"{s}_sol_per_hour": sol / hours[s], f"{s}_net_sol_per_hour": sol / hours[s] - sent.height * L * FEE / SOL / hours[s],
                            f"{s}_send_mix": {k: v / max(1, sent.height) for k, v in mix.items()}})
            grid.append(row)
    best = {}
    for b in BUDGETS:
        rows = sorted([r for r in grid if r["budget_per_hour"] == b], key=lambda r: (-r["val_sol_per_hour"], r["L"]))
        best[str(b)] = rows[0]
    total_test = float(pos_vt.filter(pl.col("split") == "test")["best_net_profit"].sum()) / SOL / hours["test"]

    # Write the bundle.
    common = {"schema": BUNDLE_SCHEMA, "labels_run": "labeller-week-20260925-001", "ml_run": "run-001",
              "label_universe": "3 venues (Raydium CLMM, Orca Whirlpool, Meteora DLMM), WSOL 2-5 hop cycles, ml-witness-2 lower-bound profit",
              "target": "POSITIVE=1; CERTIFIED_NEGATIVE and NO_LABEL=0 (NO_LABEL subsampled 1/8, weight 8); UNRESOLVED/ENTRY_DEPENDENT excluded",
              "windows": W.windows, "feature_order": cols}
    arb_meta = models.save_model(cm, ce, args.out, "arb_classifier", models.ARB_PARAMS, common)
    size_meta = models.save_model(sm, se, args.out, "size_model", {**models.ARB_PARAMS, "objective": "regression"}, common)
    stats.write_parquet(args.out / "pool_stats.parquet")
    pool_routes.write_parquet(args.out / "pool_routes.parquet")
    routes.filter(pl.col("route_id").is_in(pool_routes["route"].unique())).with_columns(
        pl.col("market_ids").str.split(",").list.len().alias("hops")).write_parquet(args.out / "routes.parquet")
    (args.out / "thresholds.json").write_text(json.dumps({b: {"ev_threshold": r["ev_threshold"], "L": r["L"],
                                                              "routes_per_decision": ROUTES_PER_PACK * r["L"]} for b, r in best.items()}, indent=1))
    (args.out / "eval.json").write_text(json.dumps({"quality": quality, "operating_points": best, "grid": grid,
                                                    "test_total_sol_per_hour": total_test, "hours": hours}, indent=1, default=str))
    manifest = {**common, "created": datetime.now(timezone.utc).isoformat(), "models": {"arb_classifier": arb_meta, "size_model": size_meta},
                "features": {"txflat_v2": prep.tx_features, "extras": EXTRA_NUM, "pool_priors": PRIOR},
                "lookup": {"routes_per_pack": ROUTES_PER_PACK, "max_routes_per_pool": max(DEPTH_ROUTES),
                           "candidate_rule": "union of pool_routes for every labelled-venue pool the tx names (ALT-resolved), ranked by support desc then route id"},
                "files": sorted(p.name for p in args.out.iterdir())}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=1, default=str))
    print(f"test labelled opportunity {total_test:.4f} SOL/h")
    for b, r in best.items():
        print(b, "L", r["L"], "test dec/h %.0f cap %d SOL/h %.4f net %.4f" % (r["test_decisions_per_hour"], r["test_captured"],
                                                                         r["test_sol_per_hour"], r["test_net_sol_per_hour"]))


if __name__ == "__main__":
    main()
