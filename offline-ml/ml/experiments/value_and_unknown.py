"""Two follow-ups to the weekly v1 run (same frozen library, same splits, same arb classifier).

1. Value-weighted policies. A size model (LightGBM regression of log net profit, trained on POSITIVE train
   transactions, early-stopped on validation) turns P(arb) into expected value per send:
       EV = P(arb) * E[profit | arb],   joint EV = max over packs P(cover) * EV.
   Thresholds are fitted on validation to send budgets (as in the notebook); the depth / live top-L per
   budget is the one with the most validation SOL captured. Reported on test: SOL captured per chain hour,
   gross and net of a 5,000-lamport base fee per send, against the count-based joint policy.

2. UNKNOWN. Why transactions are UNKNOWN (labeller reasons), how arb-like each group looks to the classifier,
   which groups fill the send budgets, and a bounded estimate of hidden arbs: UNKNOWN transactions in each
   arb-score bin are assumed to be positive at the labelled rate of that bin (an assumption, not a label).

    python experiments/value_and_unknown.py --week /data/.../week/run-001 --run /data/.../runs/v1-week-...

Captured value of a success is the transaction's best labelled net profit (all bases in this run are SOL).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import polars as pl
import lightgbm as lgb

from firm_ml import encoding, models, policy, splits, week

FEE_LAMPORTS = 5_000
BUDGETS = [10, 30, 100, 250, 1000, 4000, 16000]
DEPTHS = [3, 16, 64]
LIVE_TOP = [1, 3]
SOL = 1e9


def size_model(tx: pl.DataFrame, prep, threads: int):
    pos = tx.filter((pl.col("arb_target") == 1) & (pl.col("best_net_profit") > 0))
    enc = encoding.FeatureEncoder(prep.tx_features, prep.kinds).fit(pos.filter(pl.col("split") == "train"))
    part = {s: pos.filter(pl.col("split") == s) for s in splits.SPLITS}
    y = {s: np.log(part[s]["best_net_profit"].to_numpy().astype(np.float64)) for s in splits.SPLITS}
    cat = [enc.columns.index(c) for c in enc.categorical]
    params = {**models.ARB_PARAMS, "objective": "regression", "metric": ["l2"], "num_threads": threads}
    dtr = lgb.Dataset(enc.transform(part["train"]), y["train"], feature_name=enc.columns, categorical_feature=cat, free_raw_data=False)
    dva = lgb.Dataset(enc.transform(part["val"]), y["val"], reference=dtr, categorical_feature=cat, free_raw_data=False)
    m = lgb.train(params, dtr, num_boost_round=3000, valid_sets=[dva], valid_names=["val"],
                  callbacks=[lgb.early_stopping(100, first_metric_only=True, verbose=False)])
    quality = {}
    for s in ("val", "test"):
        p = m.predict(enc.transform(part[s]), num_iteration=m.best_iteration)
        a = y[s]
        rank = lambda v: pl.Series(v).rank().to_numpy()
        top = a >= np.quantile(a, 0.99)
        quality[s] = {"n": len(a), "spearman": float(np.corrcoef(rank(p), rank(a))[0, 1]),
                      "rmse_log": float(np.sqrt(np.mean((p - a) ** 2))),
                      "baseline_rmse_log": float(np.sqrt(np.mean((y["train"].mean() - a) ** 2))),
                      "top1pct_in_predicted_top5pct": float(np.mean(p[top] >= np.quantile(p, 0.95)))}
    return m, enc, quality


def captured(d: pl.DataFrame, threshold: float, value: pl.DataFrame) -> tuple[int, float]:
    ok = (pl.col("arb_target") == 1) & (pl.col("encode_status") == 0) & (pl.col("relevant") == 1) & \
         (pl.col("_s") >= threshold) & pl.col("coverable") & (pl.col("hit") == 1)
    got = d.filter(ok).select("master_tx_id").join(value, on="master_tx_id", how="left")
    return got.height, float(got["best_net_profit"].fill_null(0).sum()) / SOL


def run_policies(tx_eval, pairs, value, hours, ppm, out):
    weight = policy.sample_weight(tx_eval, ppm)
    # name -> (decision rule, tx score column used as arb_score, pack score, joint prob)
    pols = {"count_joint": ("joint", "arb_score", "ml", "ml"),
            "ev_two_stage": ("two_stage", "ev", "ml", None),
            "ev_joint": ("joint", "ev", "ml", "ml")}
    rows = []
    for depth in DEPTHS:
        # Every policy orders packs by the learned ranker: one sort per (split, depth) serves all of them.
        r = {s: policy.ranked(pairs.filter(pl.col("split") == s), "ml", depth) for s in ("val", "test")}
        for name, (rule, txscore, pscore, joint) in pols.items():
            t = {s: tx_eval.filter(pl.col("split") == s).with_columns(pl.col(txscore).alias("arb_score")) for s in ("val", "test")}
            for L in LIVE_TOP:
                d = {}
                for s in ("val", "test"):
                    x = policy.per_tx(t[s], None, pscore, depth, L, joint, ranked_pairs=r[s])
                    d[s] = x.with_columns(policy.decision_score(x, rule).alias("_s"), weight.alias("_w"))
                sv, wv = d["val"]["_s"].to_numpy(), d["val"]["_w"].to_numpy()
                for budget in BUDGETS:
                    th = policy.threshold_for_budget(sv, wv, budget * hours["val"])
                    row = {"policy": name, "depth": depth, "L": L, "budget_per_hour": budget, "threshold": th}
                    for s in ("val", "test"):
                        sent = d[s].filter(pl.col("_s") >= th)
                        n, sol = captured(d[s], th, value)
                        sends_h = float(sent["_w"].sum()) / hours[s]
                        row.update({f"{s}_sends_per_hour": sends_h, f"{s}_captured": n, f"{s}_sol_per_hour": sol / hours[s],
                                    f"{s}_net_sol_per_hour_basefee": sol / hours[s] - sends_h * FEE_LAMPORTS / SOL,
                                    f"{s}_unknown_share": 1 - sent.filter(pl.col("arb_target").is_not_null()).height / max(1.0, float(sent["_w"].sum()))})
                    rows.append(row)
                print(name, depth, L, "done", flush=True)
    grid = pl.DataFrame(rows)
    best = (grid.sort(["policy", "budget_per_hour", "val_sol_per_hour", "L", "depth"], descending=[False, False, True, False, False])
                .group_by(["policy", "budget_per_hour"], maintain_order=True).first())
    grid.write_parquet(out / "value_policy_grid.parquet")
    return best


def unknown_study(tx: pl.DataFrame, reasons: pl.DataFrame, hours: dict) -> dict:
    t = tx.filter(pl.col("split") != "train").join(reasons, on="master_tx_id", how="left").with_columns(
        pl.when(pl.col("arb_label") == "UNKNOWN").then(pl.col("unknown_reasons")).otherwise(pl.col("arb_label")).alias("group"))
    res = {}
    res["by_group"] = (t.group_by("split", "group").agg(
        pl.len().alias("txs"), pl.col("relevant").mean().alias("relevant_share"),
        pl.col("arb_score").median().alias("median_arb_score"),
        (pl.col("arb_score") >= 0.5).mean().alias("share_score_ge_0_5"),
        (pl.col("arb_score") >= 0.9).mean().alias("share_score_ge_0_9"))
        .sort("split", "txs", descending=[False, True]).to_dicts())
    # Hidden-arb estimate (test, relevant only): labelled positive rate and mean profit per arb-score decile of
    # labelled relevant txs, applied to UNKNOWN relevant txs of the same decile. Assumption: UNKNOWN is missing
    # at random given the score. NO_LABEL is reported separately (never evaluated by the labeller).
    test = t.filter((pl.col("split") == "test") & (pl.col("relevant") == 1))
    breaks = np.unique(np.quantile(test.filter(pl.col("arb_target").is_not_null())["arb_score"].to_numpy(), np.linspace(0, 1, 11)[1:-1]))
    test = test.with_columns(pl.col("arb_score").cut(list(breaks), labels=[f"d{i}" for i in range(len(breaks) + 1)]).alias("decile"))
    lab = test.filter(pl.col("arb_target").is_not_null()).group_by("decile").agg(
        pl.col("arb_target").mean().alias("pos_rate"), pl.col("best_net_profit").filter(pl.col("arb_target") == 1).mean().alias("mean_profit"),
        pl.len().alias("labelled"))
    unk = test.filter(pl.col("arb_label") == "UNKNOWN").group_by("decile", "unknown_reasons").agg(pl.len().alias("unknown"))
    est = unk.join(lab, on="decile", how="left").with_columns(
        (pl.col("unknown") * pl.col("pos_rate")).alias("est_hidden_arbs"),
        (pl.col("unknown") * pl.col("pos_rate") * pl.col("mean_profit") / SOL).alias("est_hidden_sol"))
    res["hidden_estimate_by_reason"] = est.group_by("unknown_reasons").agg(
        pl.col("unknown").sum(), pl.col("est_hidden_arbs").sum(), pl.col("est_hidden_sol").sum()).with_columns(
        (pl.col("est_hidden_sol") / hours["test"]).alias("est_hidden_sol_per_hour")).sort("unknown", descending=True).to_dicts()
    res["labelled_test"] = {"positives": int((test["arb_target"] == 1).sum()),
                            "sol": float(test["best_net_profit"].fill_null(0).filter(test["arb_target"] == 1).sum()) / SOL}
    res["labelled_test"]["sol_per_hour"] = res["labelled_test"]["sol"] / hours["test"]
    res["deciles"] = lab.sort("decile").to_dicts()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--run", required=True, type=Path)
    ap.add_argument("--threads", type=int, default=16)
    args = ap.parse_args()
    out = args.run / "experiments"
    out.mkdir(exist_ok=True)
    W = week.open_week(args.week)
    prep = W.prep
    ppm = prep.manifest["config"]["unknown_pair_ppm"]
    metrics = json.loads((args.run / "metrics.json").read_text())
    hours = {t["split"]: t["chain_hours"] for t in metrics["splits"]["table"]}

    tx = splits.assign_split(week.load_transactions(W), W.bounds)
    reasons = pl.concat([week._rekey(pl.concat([pl.read_parquet(p.root / "tx_labels" / n, columns=["master_tx_id", "unknown_reasons"])
                                                 for n in p.manifest["labels"]["parts"]]), W.index(p)).select("master_tx_id", "unknown_reasons")
                         for p in W.parts])
    preds = pl.read_parquet(args.run / "predictions" / "arb_predictions.parquet", columns=["master_tx_id", "arb_score"])
    tx = tx.join(preds, on="master_tx_id", how="left")      # the run's own classifier scores (val/test)
    value = tx.filter(pl.col("arb_target") == 1).select("master_tx_id", "best_net_profit")

    m, enc, quality = size_model(tx, prep, args.threads)
    print("size model", json.dumps(quality), flush=True)
    ev_tx = tx.filter(pl.col("split") != "train")
    size = np.concatenate([m.predict(enc.transform(c), num_iteration=m.best_iteration) for c in ev_tx.iter_slices(1_000_000)])
    tx_eval = ev_tx.select("master_tx_id", "arb_target", "arb_label", "encode_status", "relevant", "arb_score", "pair_sample",
                           "min_cover_rank_live", "split").with_columns(
        (pl.col("arb_score") * pl.Series(np.exp(size))).alias("ev"))

    res = {"size_model": {"best_iteration": m.best_iteration, "quality": quality,
                          "importance": models.importance(m)[:20]}}
    res["unknown"] = unknown_study(ev_tx, reasons, hours)
    print("unknown study done", flush=True)

    pairs = pl.read_parquet(args.run / "predictions" / "pack_predictions.parquet",
                            columns=["master_tx_id", "split", "pack_id", "pack_rank", "pack_target", "ml"]).with_columns(
        pl.lit(True).alias("eligible_live"))
    best = run_policies(tx_eval, pairs, value, hours, ppm, out)
    res["operating_points"] = best.to_dicts()
    total_test = float(value.join(tx.select("master_tx_id", "split"), on="master_tx_id").filter(pl.col("split") == "test")["best_net_profit"].sum()) / SOL
    res["test_total_sol_per_hour"] = total_test / hours["test"]
    (out / "value_and_unknown.json").write_text(json.dumps(res, indent=1, default=str))

    print(f"\ntest labelled opportunity: {res['test_total_sol_per_hour']:.4f} SOL/h")
    print(best.select("policy", "budget_per_hour", "depth", "L", "test_sends_per_hour", "test_captured", "test_sol_per_hour",
                      "test_net_sol_per_hour_basefee", "test_unknown_share").sort("budget_per_hour", "policy"))
    print(pl.DataFrame(res["unknown"]["hidden_estimate_by_reason"]))


if __name__ == "__main__":
    main()
