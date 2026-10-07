"""Levers 1 and 2 on the 3-venue weekly run (NO_LABEL as negative, as in `nolabel_negative.py`).

Lever 1 - pool lookup. Candidate routes for a transaction are the routes profitable in TRAIN that pass through a
labelled-venue pool the transaction names, ranked by train support (distinct train positive transactions where
the route was profitable; ties by route id). A send carries the top 3 * L routes (L "packs" of up to 3 routes;
pack feasibility is not checked here). Compared with the frozen library's per-key packs (availability at depth k).

Lever 2 - size and venue features from the raw transaction (`extract_extras.py`): pools named (static / all /
writable), swap sizes of direct venue swaps and Jupiter routes, WSOL presence, and per-pool train priors
(touch count, positive rate, positive count, mean log profit, route count). Priors are computed from TRAIN only;
train rows get out-of-fold priors (5 contiguous slot folds). The arb classifier and the size model are refit
with and without these features on the same rows.

Policies (test, thresholds fitted on validation to decisions per chain hour; the live top-L with the most
validation SOL per budget): EV = P(arb) * E[profit | arb] from each feature set, sent when the transaction has a
candidate. Volume is exact (every val/test transaction is scored). Net subtracts 5,000 lamports per sent
transaction (L per decision).

    python experiments/pool_levers.py --week ... --run ... --credentials tigris.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import lightgbm as lgb
import numpy as np
import polars as pl

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from nolabel_negative import keep_nolabel, SUB  # noqa: E402

from firm_ml import encoding, metrics, models, splits, week  # noqa: E402

RUN_PREFIX = "ml/labeller-week-20260925-001/run-001"
SOL = 1e9
FEE = 5_000
BUDGETS = [30, 100, 250, 1000, 4000, 16000]
LIVE_TOP = [1, 2, 3]
ROUTES_PER_PACK = 3
DEPTH_ROUTES = [3, 9, 24, 48, 96, 192]
EXTRA_NUM = ["n_pools_all", "n_pools_static", "n_pools_writable", "n_venue_swaps", "log_venue_swap", "n_jup_routes",
             "log_jup_in", "wsol_in_keys"]
PRIOR = ["prior_touch_max", "prior_pos_rate_max", "prior_pos_sum", "prior_logprofit_max", "prior_routes_max", "n_candidate_routes"]


def fetch_route_pools(week_root: Path, store, fetch, published, train_parts: list[str]) -> pl.DataFrame:
    frames = []
    for pid in train_parts:
        d = week_root / "indexes" / pid
        if not (d / "routes.parquet").exists():
            m = published(store, f"{RUN_PREFIX}/index/{pid}")
            fetch(store, [r for r in m["files"] if r["path"] == "index/routes.parquet"], d, strip="index/")
        frames.append(pl.read_parquet(d / "routes.parquet", columns=["route_id", "market_ids"]))
    return pl.concat(frames).unique("route_id")


def pool_priors(tx: pl.DataFrame, rows: pl.DataFrame) -> pl.DataFrame:
    """Per pool: touches, positives, positive rate (smoothed), mean log profit of positives, from `rows`."""
    e = rows.select("master_tx_id", "pools_all", "target3", "best_net_profit").explode("pools_all").drop_nulls("pools_all")
    return e.group_by("pools_all").agg(
        pl.len().alias("touch"), (pl.col("target3") == 1).sum().alias("pos"),
        pl.col("best_net_profit").filter(pl.col("target3") == 1).log().mean().alias("logprofit"),
    ).with_columns(((pl.col("pos") + 0.1) / (pl.col("touch") + 10)).alias("pos_rate")).rename({"pools_all": "pool"})


def attach_priors(target: pl.DataFrame, priors: pl.DataFrame, pool_routes_n: pl.DataFrame) -> pl.DataFrame:
    e = (target.select("master_tx_id", "pools_all").explode("pools_all").rename({"pools_all": "pool"})
         .join(priors, on="pool", how="left").join(pool_routes_n, on="pool", how="left"))
    agg = e.group_by("master_tx_id").agg(
        pl.col("touch").max().fill_null(0).alias("prior_touch_max"), pl.col("pos_rate").max().fill_null(0).alias("prior_pos_rate_max"),
        pl.col("pos").sum().fill_null(0).alias("prior_pos_sum"), pl.col("logprofit").max().fill_null(0).alias("prior_logprofit_max"),
        pl.col("n_routes").max().fill_null(0).alias("prior_routes_max"), pl.col("n_routes").sum().fill_null(0).alias("n_candidate_routes"))
    return target.join(agg, on="master_tx_id", how="left").with_columns([pl.col(c).fill_null(0) for c in PRIOR])


def fit(tx, cols, kinds, target, weight, threads, objective="binary"):
    enc = encoding.FeatureEncoder(cols, kinds).fit(tx.filter(pl.col("split") == "train"))
    tr, va = (tx.filter(pl.col("split") == s) for s in ("train", "val"))
    cat = [enc.columns.index(c) for c in enc.categorical]
    params = {**models.ARB_PARAMS, "num_threads": threads}
    if objective == "regression":
        params.update(objective="regression", metric=["l2"])
    dtr = lgb.Dataset(enc.transform(tr), tr[target].to_numpy(), weight=tr[weight].to_numpy() if weight else None,
                      feature_name=enc.columns, categorical_feature=cat, free_raw_data=False)
    dva = lgb.Dataset(enc.transform(va), va[target].to_numpy(), weight=va[weight].to_numpy() if weight else None,
                      reference=dtr, categorical_feature=cat, free_raw_data=False)
    m = lgb.train(params, dtr, num_boost_round=3000, valid_sets=[dva], valid_names=["val"],
                  callbacks=[lgb.early_stopping(100, first_metric_only=True, verbose=False)])
    return m, enc


def predict(m, enc, df):
    return np.concatenate([m.predict(enc.transform(c), num_iteration=m.best_iteration) for c in df.iter_slices(1_000_000)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--run", required=True, type=Path)
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--threads", type=int, default=16)
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE.parent / "weekly"))
    if not (HERE.parent / "weekly" / "fleet").exists():  # local checkout: fleet/ lives in the labeller repo
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, fetch, published

    out = args.run / "experiments"
    W = week.open_week(args.week)
    prep = W.prep
    hours = {t["split"]: t["chain_hours"] for t in json.loads((args.run / "metrics.json").read_text())["splits"]["table"]}
    res = {}

    # ---- transactions, labels (NO_LABEL negative), extras
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
    assert tx["pools_all"].null_count() == 0, "extras missing for some transactions"
    print("loaded", tx.height, flush=True)

    # ---- lever 1: route support from train positives, routes -> pools
    store = Store(workers=16)
    train_parts = [p.root.name for p in W.parts if p.manifest["config"]["min_slot"] <= W.bounds["train"][1]]
    routes = fetch_route_pools(args.week, store, fetch, published, train_parts)
    sup = (tx.filter((pl.col("split") == "train") & (pl.col("target3") == 1)).select("master_tx_id", pl.col("profitable_routes").str.split(","))
           .explode("profitable_routes").filter(pl.col("profitable_routes") != "")
           .group_by("profitable_routes").agg(pl.col("master_tx_id").n_unique().alias("support")).rename({"profitable_routes": "route"}))
    pool_routes = (sup.join(routes.rename({"route_id": "route"}), on="route", how="inner")
                   .with_columns(pl.col("market_ids").str.split(",").alias("pool")).explode("pool").select("pool", "route", "support"))
    pool_routes_n = pool_routes.group_by("pool").agg(pl.len().alias("n_routes"))
    # The global top-k over several pools is inside the union of each pool's top-k: keep the top max(k) per pool.
    pool_routes = (pool_routes.sort(["pool", "support", "route"], descending=[False, True, False])
                   .with_columns(pl.int_range(pl.len()).over("pool").alias("_r")).filter(pl.col("_r") < max(DEPTH_ROUTES)).drop("_r"))
    res["lever1_index"] = {"train_routes_with_support": sup.height, "pools_with_routes": pool_routes_n.height}
    print("pool->routes", res["lever1_index"], flush=True)

    def min_hit_rank(df: pl.DataFrame, pools_col: str) -> pl.DataFrame:
        """Rank (0-based) of the first profitable route among a transaction's ranked candidates."""
        c = (df.select("master_tx_id", pl.col(pools_col).alias("pool")).explode("pool").drop_nulls("pool")
             .join(pool_routes, on="pool", how="inner").group_by("master_tx_id", "route").agg(pl.col("support").max())
             .sort(["master_tx_id", "support", "route"], descending=[False, True, False])
             .with_columns(pl.int_range(pl.len()).over("master_tx_id").alias("rank")))
        prof = df.select("master_tx_id", pl.col("profitable_routes").str.split(",").alias("route")).explode("route")
        return (c.join(prof, on=["master_tx_id", "route"], how="inner").group_by("master_tx_id").agg(pl.col("rank").min().alias(f"hit_rank_{pools_col}")))

    vt = tx.filter(pl.col("split") != "train")
    pos_vt = vt.filter(pl.col("target3") == 1)
    for col in ("pools_all", "pools_static"):
        pos_vt = pos_vt.join(min_hit_rank(pos_vt, col), on="master_tx_id", how="left")
    rows = []
    for s in ("val", "test"):
        p = pos_vt.filter(pl.col("split") == s)
        tot_v = float(p["best_net_profit"].sum())
        for col in ("pools_all", "pools_static"):
            for k in DEPTH_ROUTES:
                hit = p.filter(pl.col(f"hit_rank_{col}") < k)
                rows.append({"split": s, "lookup": f"pool:{col}", "k_routes": k, "available": hit.height / p.height,
                             "value_available": float(hit["best_net_profit"].sum()) / tot_v})
        for kp in (1, 3, 8, 16, 32, 64):
            hit = p.filter(pl.col("min_cover_rank_live") < kp)
            rows.append({"split": s, "lookup": "library_live_packs", "k_routes": 3 * kp, "available": hit.height / p.height,
                         "value_available": float(hit["best_net_profit"].sum()) / tot_v})
    res["availability"] = rows
    print(pl.DataFrame(rows).filter(pl.col("split") == "test"), flush=True)

    # ---- lever 2: priors (train out-of-fold, val/test from all train) and models
    tr = tx.filter(pl.col("split") == "train")
    edges = np.quantile(tr["slot"].to_numpy(), np.linspace(0, 1, 6))
    tr = tr.with_columns(pl.col("slot").cut(list(edges[1:-1]), labels=[str(i) for i in range(5)]).alias("fold"))
    parts = []
    for f in range(5):
        parts.append(attach_priors(tr.filter(pl.col("fold") == str(f)), pool_priors(tx, tr.filter(pl.col("fold") != str(f))), pool_routes_n))
    full = pool_priors(tx, tr)
    tx = pl.concat([pl.concat(parts).drop("fold"), attach_priors(vt, full, pool_routes_n)], how="vertical_relaxed")
    del tr, parts, vt
    kinds = {**prep.kinds, **{c: "numeric" for c in EXTRA_NUM + PRIOR}}
    lab = tx.filter(pl.col("target3").is_not_null() & (pl.col("encode_status") == 0))
    lab = lab.filter((pl.col("group") != "NO_LABEL") | keep_nolabel(pl.col("master_tx_id"))).with_columns(
        pl.when(pl.col("group") == "NO_LABEL").then(float(SUB)).otherwise(1.0).alias("w"))
    pos = tx.filter((pl.col("target3") == 1) & (pl.col("best_net_profit") > 0)).with_columns(pl.col("best_net_profit").cast(pl.Float64).log().alias("logp"))
    feats = {"base": prep.tx_features, "new": prep.tx_features + EXTRA_NUM + PRIOR}
    ev_tx = tx.filter(pl.col("split") != "train")
    quality = {}
    for name, cols in feats.items():
        cm, ce = fit(lab, cols, kinds, "target3", "w", args.threads)
        sm, se = fit(pos, cols, kinds, "logp", None, args.threads, "regression")
        p_arb, p_size = predict(cm, ce, ev_tx), predict(sm, se, ev_tx)
        ev_tx = ev_tx.with_columns(pl.Series(f"arb_{name}", p_arb), (pl.Series(p_arb) * np.exp(p_size)).alias(f"ev_{name}"))
        q = {}
        for s in ("val", "test"):
            d = ev_tx.filter((pl.col("split") == s) & pl.col("target3").is_not_null())
            c = metrics.classifier_summary(d["target3"].to_numpy(), d[f"arb_{name}"].to_numpy())
            ps = pos.filter(pl.col("split") == s)
            sp = predict(sm, se, ps)
            a = ps["logp"].to_numpy()
            rk = lambda v: pl.Series(v).rank().to_numpy()
            top = a >= np.quantile(a, 0.99)
            q[s] = {"arb_pr_auc": c["pr_auc"], "arb_roc_auc": c["roc_auc"], "size_spearman": float(np.corrcoef(rk(sp), rk(a))[0, 1]),
                    "top1pct_in_pred_top5pct": float(np.mean(sp[top] >= np.quantile(sp, 0.95)))}
        quality[name] = {**q, "arb_importance": models.importance(cm)[:15], "size_importance": models.importance(sm)[:15]}
        print(name, json.dumps(q), flush=True)
    res["models"] = quality

    # ---- policies on pool lookup
    hr = pos_vt.select("master_tx_id", "hit_rank_pools_all")
    cand = (ev_tx.select("master_tx_id", pl.col("pools_all").alias("pool")).explode("pool").drop_nulls("pool")
            .join(pool_routes_n, on="pool", how="inner").select("master_tx_id").unique().with_columns(pl.lit(True).alias("has_candidate")))
    d = (ev_tx.select("master_tx_id", "split", "target3", "group", "best_net_profit", "encode_status", "ev_base", "ev_new")
         .join(cand, on="master_tx_id", how="left").join(hr, on="master_tx_id", how="left")
         .with_columns(pl.col("has_candidate").fill_null(False)))
    rows = []
    for feat in ("base", "new"):
        for L in LIVE_TOP:
            score = {s: d.filter(pl.col("split") == s).with_columns(
                pl.when(pl.col("has_candidate") & (pl.col("encode_status") == 0)).then(pl.col(f"ev_{feat}")).otherwise(float("-inf")).alias("_s"))
                for s in ("val", "test")}
            sv = np.sort(score["val"]["_s"].to_numpy())[::-1]
            for budget in BUDGETS:
                n = int(budget * hours["val"])
                th = float(sv[n - 1]) if 0 < n <= len(sv) and np.isfinite(sv[n - 1]) else float("inf")
                row = {"features": feat, "lookup": "pool:pools_all", "L": L, "budget_per_hour": budget, "threshold": th}
                for s in ("val", "test"):
                    x = score[s]
                    sent = x.filter(pl.col("_s") >= th)
                    hit = sent.filter((pl.col("target3") == 1) & (pl.col("hit_rank_pools_all") < ROUTES_PER_PACK * L))
                    sol = float(hit["best_net_profit"].sum()) / SOL
                    comp = dict(sent.group_by("group").len().iter_rows())
                    row.update({f"{s}_decisions_per_hour": sent.height / hours[s], f"{s}_captured": hit.height, f"{s}_sol_per_hour": sol / hours[s],
                                f"{s}_net_sol_per_hour": sol / hours[s] - sent.height * L * FEE / SOL / hours[s],
                                **{f"{s}_share_{g}": comp.get(g, 0) / max(1, sent.height) for g in ("POSITIVE", "NO_LABEL", "UNRESOLVED")}})
                rows.append(row)
    grid = pl.DataFrame(rows)
    grid.write_parquet(out / "pool_levers_grid.parquet")
    best = (grid.sort(["features", "budget_per_hour", "val_sol_per_hour", "L"], descending=[False, False, True, False])
                .group_by(["features", "budget_per_hour"], maintain_order=True).first())
    res["operating_points"] = best.to_dicts()
    res["test_total_sol_per_hour"] = float(pos_vt.filter(pl.col("split") == "test")["best_net_profit"].sum()) / SOL / hours["test"]
    (out / "pool_levers.json").write_text(json.dumps(res, indent=1, default=str))
    print(f"test labelled opportunity {res['test_total_sol_per_hour']:.4f} SOL/h")
    print(best.select("features", "budget_per_hour", "L", "test_decisions_per_hour", "test_captured", "test_sol_per_hour",
                      "test_net_sol_per_hour", "test_share_POSITIVE", "test_share_UNRESOLVED").sort("budget_per_hour", "features"))


if __name__ == "__main__":
    main()
