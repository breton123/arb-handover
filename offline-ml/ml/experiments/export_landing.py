"""Export the landing head P(landed_ok) as bundle `landing-v1`.

Same data, features and protocol as `landing_head.py` (the `all` model), minus the two block-position inputs
(`entry_index`, `tx_index`: low importance, awkward to compute live when shreds are missing). Train 70% / val 15%
(early stopping and cut-offs) / test 15% (reported once), by slot.

Bundle (`--out`):
    manifest.json              schema, feature order and kinds, hash function, reputation parameters, population rule,
                               windows, metrics, cut-off table (fitted on val), file list
    landing_model.{txt,json,encoder.json}   LightGBM text model, metadata, hash vocabularies
    pools.parquet              labelled-venue pool universe: pool, program, token_a, token_b
    constants.json             venue programs, Jupiter v6, WSOL, Jito tip accounts, swap discriminators
    eval.json                  val/test metrics, gate trade-off (val and test), calibration, subsets, importance
    parity_model.parquet       50,000 test rows: every model input (raw values) and the expected p_land
    parity_features.parquet    fixture-window transactions (slots of orbitflare-20260927-002844.cap): signature and
                               the non-reputation inputs, for checking a feature builder against the frozen capture

    python experiments/export_landing.py --week ... --partitions ID ... --fixture-partition ID --out .../landing-v1
"""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import polars as pl

import sys
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from extract_extras import JUP6, SWAP_NAMES, VENUES, WSOL  # noqa: E402
from extract_population import JITO_TIPS  # noqa: E402
from landing_head import HALF_LIFE, LAG, HASH, NUM, REP, reputation, summary  # noqa: E402

from firm_ml import encoding, models  # noqa: E402

import lightgbm as lgb  # noqa: E402

SCHEMA = "firm-landing-bundle-v1"
NUM_EXPORT = [c for c in NUM if c not in ("entry_index", "tx_index")]
COLS = HASH + NUM_EXPORT + REP
FIXTURE_SLOTS = (450835794, 450836600)   # orbitflare-20260927-002844.cap (TXFRAME fixture), generous upper bound


def gate_table(y, p, cutoffs=(0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9)):
    return [{"cutoff": c, "kept": float((p >= c).mean()), "landed_rate_kept": float(y[p >= c].mean()) if (p >= c).any() else None,
             "failures_removed": float(((1 - y) * (p < c)).sum() / max(1, (1 - y).sum())),
             "landed_lost": float((y * (p < c)).sum() / max(1, y.sum()))} for c in cutoffs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--partitions", nargs="+", required=True)
    ap.add_argument("--fixture-partition")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--threads", type=int, default=16)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    df = pl.concat([pl.read_parquet(args.week / "population" / f"{p}.parquet") for p in args.partitions], how="vertical_relaxed")
    df = reputation(df.sort("slot", "entry_index", "tx_index").with_columns(pl.col("landed_ok").cast(pl.Int8).alias("y")))
    slots = np.sort(df["slot"].unique().to_numpy())
    c1, c2 = int(slots[int(len(slots) * 0.70)]), int(slots[int(len(slots) * 0.85)])
    df = df.with_columns(pl.when(pl.col("slot") < c1).then(pl.lit("train")).when(pl.col("slot") < c2).then(pl.lit("val"))
                         .otherwise(pl.lit("test")).alias("split"))
    kinds = {**{c: "hash" for c in HASH}, **{c: "numeric" for c in NUM_EXPORT + REP}}
    tr, va, te = (df.filter(pl.col("split") == s) for s in ("train", "val", "test"))
    enc = encoding.FeatureEncoder(COLS, kinds).fit(tr)
    cat = [enc.columns.index(c) for c in enc.categorical]
    params = {**models.ARB_PARAMS, "num_threads": args.threads, "metric": ["binary_logloss", "auc"], "num_leaves": 63, "min_data_in_leaf": 200}
    dtr = lgb.Dataset(enc.transform(tr), tr["y"].to_numpy(), feature_name=enc.columns, categorical_feature=cat, free_raw_data=False)
    dva = lgb.Dataset(enc.transform(va), va["y"].to_numpy(), reference=dtr, categorical_feature=cat, free_raw_data=False)
    m = lgb.train(params, dtr, num_boost_round=4000, valid_sets=[dva], valid_names=["val"],
                  callbacks=[lgb.early_stopping(100, first_metric_only=True, verbose=False)])
    pv = m.predict(enc.transform(va), num_iteration=m.best_iteration)
    pt = m.predict(enc.transform(te), num_iteration=m.best_iteration)
    yv, yt = va["y"].to_numpy(), te["y"].to_numpy()
    ev = {"val": summary(yv, pv), "test": summary(yt, pt), "gate_val": gate_table(yv, pv), "gate_test": gate_table(yt, pt),
          "calibration_test": [{"bin": f"{lo:.1f}-{lo + 0.1:.1f}", "n": int(((pt >= lo) & (pt < lo + 0.1)).sum()),
                                "landed": float(yt[(pt >= lo) & (pt < lo + 0.1)].mean()) if ((pt >= lo) & (pt < lo + 0.1)).any() else None}
                               for lo in np.arange(0, 1, 0.1)],
          "importance": models.importance(m)[:25], "best_iteration": m.best_iteration,
          "rows": {"train": tr.height, "val": va.height, "test": te.height}}
    bad = (te["rep_total"].to_numpy() >= 20) & (np.nan_to_num(te["rep_fail_rate"].to_numpy()) >= 0.9)
    for sub, mask in {"attempted_arb": te["attempted_arb_static"].to_numpy() == 1, "bad_payers": bad,
                      "jupiter": te["n_jup_routes"].to_numpy() > 0, "has_venue_swap": te["n_venue_swaps"].to_numpy() > 0}.items():
        if mask.sum() > 100 and 0 < yt[mask].mean() < 1:
            ev[f"subset_{sub}_test"] = summary(yt[mask], pt[mask])
    print("val", ev["val"], "test", ev["test"], flush=True)

    win = {"train": [int(slots[0]), c1 - 1], "val": [c1, c2 - 1], "test": [c2, int(slots[-1])]}
    common = {"schema": SCHEMA, "target": "landed_ok: executed without error (runtime success), among transactions in finalized blocks",
              "windows_slots": win, "partitions": args.partitions, "feature_order": COLS}
    meta = models.save_model(m, enc, args.out, "landing_model", params, common)

    # Static tables the live feature builder needs.
    mk = pl.concat([pl.read_parquet(f) for f in (args.week / "markets").glob("*/markets.parquet")]).unique("pool")
    mk.select("pool", "program", "token_a", "token_b").sort("pool").write_parquet(args.out / "pools.parquet")
    swap_disc = {n: hashlib.sha256(f"global:{n}".encode()).digest()[:8].hex() for n in SWAP_NAMES}
    (args.out / "constants.json").write_text(json.dumps({
        "venue_programs": sorted(VENUES), "jupiter_v6": JUP6, "wsol_mint": WSOL, "jito_tip_accounts": sorted(JITO_TIPS),
        "system_program": "11111111111111111111111111111111", "compute_budget_program": "ComputeBudget111111111111111111111111111111",
        "venue_swap_discriminators_hex": swap_disc}, indent=1))

    # Parity sets.
    rng = np.random.default_rng(7)
    idx = np.sort(rng.choice(te.height, size=min(50_000, te.height), replace=False))
    te.select("signature", "slot", "fee_payer", *COLS)[idx].with_columns(pl.Series("p_land", pt[idx])).write_parquet(args.out / "parity_model.parquet")
    if args.fixture_partition:
        fx = pl.read_parquet(args.week / "population" / f"{args.fixture_partition}.parquet").filter(pl.col("slot").is_between(*FIXTURE_SLOTS))
        fx.select("signature", "slot", "entry_index", "tx_index", "fee_payer", "landed_ok", *HASH, *NUM_EXPORT).write_parquet(args.out / "parity_features.parquet")
        ev["parity_features_rows"] = fx.height

    (args.out / "eval.json").write_text(json.dumps(ev, indent=1, default=str))
    manifest = {**common, "created": datetime.now(timezone.utc).isoformat(),
                "model": meta,
                "features": {"hash": HASH, "numeric": NUM_EXPORT, "reputation": REP,
                             "hash_function": "blake2b(utf8(text), digest_size=8) read little-endian as u64, then >> 1",
                             "hash_inputs": {"prog0": "program id of top-level instruction 0", "prog1": "instruction 1", "prog2": "instruction 2",
                                             "prog_last": "last instruction", "prog_seq": "'|'.join of top-level program ids excluding ComputeBudget"},
                             "missing_hash": "0 when the instruction does not exist"},
                "reputation": {"key": "fee payer (static key 0)", "events": "landed / failed outcome of every population transaction",
                               "visibility": f"an event at slot t counts for a transaction at slot s only if t <= s - {LAG + 1}",
                               "decay": f"counts multiplied by 0.5 for every boundary of floor(slot / {HALF_LIFE}) crossed",
                               "rep_fail_rate": "rep_failed / rep_total, NaN (missing) when rep_total == 0",
                               "rep_seen": "1 if the payer has any visible event, else 0"},
                "population": "non-vote transactions whose ALT-resolved keys include a pool in pools.parquet",
                "metrics": {"val": ev["val"], "test": ev["test"]},
                "files": sorted(p.name for p in args.out.iterdir())}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=1, default=str))
    print("exported", args.out, flush=True)


if __name__ == "__main__":
    main()
