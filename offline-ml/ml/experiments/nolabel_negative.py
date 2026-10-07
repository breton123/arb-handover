"""The 3-venue question with NO_LABEL as a negative, and stricter raw gates.

The labeller writes a label row only when a transaction changes the modelled economics of a market on an
enabled venue (Raydium CLMM, Orca Whirlpool, Meteora DLMM); every other transaction on the tape is NO_LABEL.
Within that universe a NO_LABEL transaction cannot create an arb, so here it is a negative:

    target = 1 POSITIVE, 0 CERTIFIED_NEGATIVE or NO_LABEL, excluded UNRESOLVED / ENTRY_DEPENDENT.

The arb classifier is retrained on that target (train split; NO_LABEL deterministically subsampled 1/8 with
weight 8 so scores stay calibrated; early stopping on validation with the same scheme). Send volume is still
estimated from the labelled rows plus the deterministic UNKNOWN pair sample (weight 1/rate), exactly as in
the notebook, so numbers are comparable with the v1 run and `value_and_unknown.py`.

Gates (raw static keys only, relevance-v1 counts): `relevant` (any tracked account or venue program, the v1
gate), `writes` (a tracked market account is writable), `writes_or_program`. The gate per budget is chosen on
validation like depth and top-L.

    python experiments/nolabel_negative.py --week ... --run ...
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import lightgbm as lgb
import numpy as np
import polars as pl

sys.path.insert(0, str(Path(__file__).resolve().parent))
from value_and_unknown import SOL, FEE_LAMPORTS, size_model  # noqa: E402

from firm_ml import encoding, metrics, models, policy, splits, week  # noqa: E402

BUDGETS = [30, 100, 250, 1000, 4000, 16000]
DEPTHS = [3, 16]
L = 3
SUB = 8   # NO_LABEL keep 1 in SUB for training, weight SUB
GATES = {
    "relevant": pl.col("relevant") == 1,
    "writes": pl.col("rel_market_writable") > 0,
    "writes_or_program": (pl.col("rel_market_writable") > 0) | (pl.col("rel_venue_program") > 0),
}


def keep_nolabel(gid: pl.Expr) -> pl.Expr:
    return gid % SUB == 0   # low bits of the global id are the partition-local id: deterministic, label-blind


def train_classifier(tx: pl.DataFrame, prep, threads: int):
    lab = tx.filter(pl.col("target3").is_not_null() & (pl.col("encode_status") == 0))
    lab = lab.filter((pl.col("group") != "NO_LABEL") | keep_nolabel(pl.col("master_tx_id"))).with_columns(
        pl.when(pl.col("group") == "NO_LABEL").then(float(SUB)).otherwise(1.0).alias("w"))
    enc = encoding.FeatureEncoder(prep.tx_features, prep.kinds).fit(lab.filter(pl.col("split") == "train"))
    tr, va = (lab.filter(pl.col("split") == s) for s in ("train", "val"))
    cat = [enc.columns.index(c) for c in enc.categorical]
    params = {**models.ARB_PARAMS, "num_threads": threads}
    dtr = lgb.Dataset(enc.transform(tr), tr["target3"].to_numpy(), weight=tr["w"].to_numpy(), feature_name=enc.columns,
                      categorical_feature=cat, free_raw_data=False)
    dva = lgb.Dataset(enc.transform(va), va["target3"].to_numpy(), weight=va["w"].to_numpy(), reference=dtr,
                      categorical_feature=cat, free_raw_data=False)
    m = lgb.train(params, dtr, num_boost_round=3000, valid_sets=[dva], valid_names=["val"],
                  callbacks=[lgb.early_stopping(100, first_metric_only=True, verbose=False)])
    return m, enc, {"train_rows": tr.height, "val_rows": va.height, "best_iteration": m.best_iteration}


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
    hours = {t["split"]: t["chain_hours"] for t in json.loads((args.run / "metrics.json").read_text())["splits"]["table"]}

    tx = splits.assign_split(week.load_transactions(W), W.bounds)
    reasons = pl.concat([week._rekey(pl.concat([pl.read_parquet(p.root / "tx_labels" / n, columns=["master_tx_id", "unknown_reasons"])
                                                 for n in p.manifest["labels"]["parts"]]), W.index(p)).select("master_tx_id", "unknown_reasons")
                         for p in W.parts])
    tx = tx.join(reasons, on="master_tx_id", how="left").with_columns(
        pl.when(pl.col("arb_label") == "UNKNOWN").then(pl.col("unknown_reasons")).otherwise(pl.col("arb_label")).alias("group"))
    tx = tx.with_columns(pl.when(pl.col("group") == "POSITIVE").then(1).when(pl.col("group").is_in(["CERTIFIED_NEGATIVE", "NO_LABEL"]))
                         .then(0).otherwise(None).cast(pl.Int8).alias("target3"))
    res = {"groups": tx.group_by("split", "group").len().sort("split", "group").to_dicts()}

    m, enc, info = train_classifier(tx, prep, args.threads)
    ev_tx = tx.filter(pl.col("split") != "train")
    score = np.concatenate([m.predict(enc.transform(c), num_iteration=m.best_iteration) for c in ev_tx.iter_slices(1_000_000)])
    ev_tx = ev_tx.with_columns(pl.Series("arb3", score))
    q = {}
    for s in ("val", "test"):
        d = ev_tx.filter((pl.col("split") == s) & pl.col("target3").is_not_null() & (pl.col("relevant") == 1))
        q[s] = metrics.classifier_summary(d["target3"].to_numpy(), d["arb3"].to_numpy())
    res["classifier"] = {**info, "relevant_eval": q, "importance": models.importance(m)[:20]}
    print("classifier", json.dumps(q, default=str), flush=True)

    # Gate pass rates by group (test).
    res["gates"] = (ev_tx.filter(pl.col("split") == "test").group_by("group").agg(
        pl.len().alias("txs"), *[e.mean().alias(f"pass_{g}") for g, e in GATES.items()]).sort("txs", descending=True).to_dicts())
    print("gates", json.dumps(res["gates"], default=str), flush=True)

    sm, senc, squality = size_model(tx, prep, args.threads)
    size = np.concatenate([sm.predict(senc.transform(c), num_iteration=sm.best_iteration) for c in ev_tx.iter_slices(1_000_000)])
    ev_tx = ev_tx.with_columns((pl.col("arb3") * pl.Series(np.exp(size))).alias("ev3"))
    value = tx.filter(pl.col("arb_target") == 1).select("master_tx_id", "best_net_profit")
    del tx

    pairs = pl.read_parquet(args.run / "predictions" / "pack_predictions.parquet",
                            columns=["master_tx_id", "split", "pack_id", "pack_rank", "pack_target", "ml"]).with_columns(
        pl.lit(True).alias("eligible_live"))
    keep = ["master_tx_id", "arb_target", "encode_status", "pair_sample", "min_cover_rank_live", "split", "group",
            "arb3", "ev3", "relevant", "rel_market_writable", "rel_venue_program"]
    base = ev_tx.select(keep)
    weight = policy.sample_weight(base, ppm)
    pols = {"nl_two_stage": ("two_stage", "arb3", None), "nl_ev_two_stage": ("two_stage", "ev3", None), "nl_ev_joint": ("joint", "ev3", "ml")}
    rows = []
    for depth in DEPTHS:
        r = {s: policy.ranked(pairs.filter(pl.col("split") == s), "ml", depth) for s in ("val", "test")}
        for gate, gexpr in GATES.items():
            for name, (rule, txscore, joint) in pols.items():
                d = {}
                for s in ("val", "test"):
                    t = base.filter(pl.col("split") == s).with_columns(pl.col(txscore).alias("arb_score"), gexpr.cast(pl.UInt8).alias("relevant"))
                    x = policy.per_tx(t, None, "ml", depth, L, joint, ranked_pairs=r[s])
                    d[s] = x.with_columns(policy.decision_score(x, rule).alias("_s"), weight.alias("_w"))
                sv, wv = d["val"]["_s"].to_numpy(), d["val"]["_w"].to_numpy()
                for budget in BUDGETS:
                    th = policy.threshold_for_budget(sv, wv, budget * hours["val"])
                    row = {"policy": name, "gate": gate, "depth": depth, "L": L, "budget_per_hour": budget, "threshold": th}
                    for s in ("val", "test"):
                        x = d[s]
                        sent = x.filter(pl.col("_s") >= th)
                        ok = (pl.col("arb_target") == 1) & (pl.col("encode_status") == 0) & (pl.col("relevant") == 1) & \
                             (pl.col("_s") >= th) & pl.col("coverable") & (pl.col("hit") == 1)
                        got = x.filter(ok).select("master_tx_id").join(value, on="master_tx_id", how="left")
                        w = float(sent["_w"].sum())
                        sol = float(got["best_net_profit"].fill_null(0).sum()) / SOL
                        comp = dict(sent.group_by("group").agg(pl.col("_w").sum()).iter_rows())
                        row.update({f"{s}_sends_per_hour": w / hours[s], f"{s}_captured": got.height, f"{s}_sol_per_hour": sol / hours[s],
                                    f"{s}_net_sol_per_hour_basefee": sol / hours[s] - w / hours[s] * FEE_LAMPORTS / SOL,
                                    **{f"{s}_send_share_{g}": (comp.get(g, 0.0) / w if w else None)
                                       for g in ("POSITIVE", "CERTIFIED_NEGATIVE", "NO_LABEL", "UNRESOLVED", "ENTRY_DEPENDENT")}})
                    rows.append(row)
                print(depth, gate, name, "done", flush=True)
    grid = pl.DataFrame(rows)
    grid.write_parquet(out / "nolabel_negative_grid.parquet")
    best = (grid.sort(["policy", "budget_per_hour", "val_sol_per_hour", "depth"], descending=[False, False, True, False])
                .group_by(["policy", "budget_per_hour"], maintain_order=True).first())
    res["operating_points"] = best.to_dicts()
    res["size_model"] = squality
    res["test_total_sol_per_hour"] = float(value.join(base.select("master_tx_id", "split"), on="master_tx_id")
                                           .filter(pl.col("split") == "test")["best_net_profit"].sum()) / SOL / hours["test"]
    (out / "nolabel_negative.json").write_text(json.dumps(res, indent=1, default=str))
    print(best.select("policy", "budget_per_hour", "gate", "depth", "test_sends_per_hour", "test_captured", "test_sol_per_hour",
                      "test_net_sol_per_hour_basefee", "test_send_share_POSITIVE", "test_send_share_NO_LABEL",
                      "test_send_share_UNRESOLVED").sort("budget_per_hour", "policy"))


if __name__ == "__main__":
    main()
