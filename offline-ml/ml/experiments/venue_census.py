"""Which programs does real trade flow touch? A census over raw transactions (all of them, not only masters).

For a sample of one partition's raw parts: per key, the number of transactions that name it (static + ALT-resolved),
that invoke it at top level, and that name it inside a Jupiter v6 transaction (Jupiter passes each hop's program
as an account, so this approximates venue usage in routed flow). Keys never invoked at top level are dropped.

    python experiments/venue_census.py --week ... --credentials tigris.json --partition ID --parts 80
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import cbor2
import polars as pl

HERE = Path(__file__).resolve().parent
RUN_PREFIX = "ml/labeller-week-20260925-001/run-001"
JUP6 = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--partition", required=True)
    ap.add_argument("--parts", type=int, default=80)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE.parent / "weekly"))
    if not (HERE.parent / "weekly" / "fleet").exists():
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, raw_records

    store = Store(workers=16)
    plan = {p["id"]: p for p in json.loads(store.get(f"{RUN_PREFIX}/PLAN.json"))["partitions"]}
    recs = [r for r in raw_records(store, plan[args.partition]["source_prefix"]) if r["path"].endswith(".transactions.parquet")]
    step = max(1, len(recs) // args.parts)
    recs = recs[::step][:args.parts]   # spread across the partition's slots
    named, top, in_jup = Counter(), Counter(), Counter()
    n_tx = n_jup = 0
    with tempfile.TemporaryDirectory(dir=args.week) as tmp, ThreadPoolExecutor(8) as dl:
        def get(r):
            local = Path(tmp) / Path(r["path"]).name
            store.verify(r, local)
            data = local.read_bytes()
            local.unlink()
            return data
        for data in dl.map(get, recs):
            for blob in pl.read_parquet(io.BytesIO(data), columns=["record_cbor"])["record_cbor"].to_list():
                rec = cbor2.loads(blob)
                keys = set(rec["resolved_keys"])
                progs = {ix["program_id"] for ix in rec["instructions"]}
                n_tx += 1
                named.update(keys)
                top.update(progs)
                if JUP6 in progs:
                    n_jup += 1
                    in_jup.update(keys)
    rows = [{"program": k, "top_level_txs": top[k], "named_txs": named[k], "named_in_jupiter_txs": in_jup[k]}
            for k in top if top[k] >= 20]
    df = pl.DataFrame(rows).with_columns(
        (pl.col("named_txs") / n_tx).alias("share_named"), (pl.col("named_in_jupiter_txs") / max(1, n_jup)).alias("share_of_jupiter_txs")
    ).sort("named_txs", descending=True)
    print(f"sampled {len(recs)} parts, {n_tx} transactions, {n_jup} via Jupiter v6")
    with pl.Config(tbl_rows=80, fmt_str_lengths=50, tbl_width_chars=200):
        print(df.head(80))
    if args.out:
        df.write_parquet(args.out)


if __name__ == "__main__":
    main()
