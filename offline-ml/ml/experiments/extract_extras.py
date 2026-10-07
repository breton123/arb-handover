"""Per master transaction: the 3-venue pools it names and the swap sizes in its instructions.

Streams each partition's raw transactions from Tigris (download a part, keep the master transactions, decode,
delete), so the full ~1.4 TB never needs local disk. Output, one file per partition:

    <week>/extras/<partition>.parquet
        master_tx_id (partition-local), pools_static, pools_all, pools_writable   list[str]
        n_venue_swaps, max_venue_swap_amount, n_jup_routes, max_jup_in_amount, wsol_in_keys

`pools_*` are pools of the labelled venues (union of every partition's labels/markets.parquet): named among
the static keys, among all resolved keys (static + address-lookup-table), and writable. Swap amounts are the
first u64 argument of Orca Whirlpool / Raydium CLMM / Meteora DLMM swap instructions (top level only: the raw
transaction has no inner instructions) and Jupiter v6 route `in_amount` (the tail layout shared by route
variants). Raw only: no labels, logs or account state.

    python experiments/extract_extras.py --week /data/.../week/run-001 --credentials tigris.json [--parallel 4]
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import cbor2
import polars as pl

HERE = Path(__file__).resolve().parent
RUN_PREFIX = "ml/labeller-week-20260925-001/run-001"
WSOL = "So11111111111111111111111111111111111111112"
JUP6 = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4"
VENUES = {"whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc", "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK",
          "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"}
SWAP_NAMES = ["swap", "swap_v2", "swap2", "swap_exact_out", "swap_exact_out2", "swap_with_price_impact",
              "swap_with_price_impact2", "two_hop_swap", "two_hop_swap_v2", "swap_router_base_in"]
SWAP_DISC = {hashlib.sha256(f"global:{n}".encode()).digest()[:8] for n in SWAP_NAMES}


SCHEMA = {"master_tx_id": pl.UInt64, "pools_static": pl.List(pl.String), "pools_all": pl.List(pl.String),
          "pools_writable": pl.List(pl.String), "n_venue_swaps": pl.UInt16, "max_venue_swap_amount": pl.UInt64,
          "n_jup_routes": pl.UInt16, "max_jup_in_amount": pl.UInt64, "wsol_in_keys": pl.Boolean}


def u64(b: bytes, at: int) -> int:
    return int.from_bytes(b[at:at + 8], "little")


def extract(rec: dict, pools: set) -> dict:
    keys = rec["resolved_keys"]
    static = set(rec["static_keys"])
    writable = rec["writable"]
    ps, pa, pw = set(), set(), set()
    for i, k in enumerate(keys):
        if k in pools:
            pa.add(k)
            if k in static:
                ps.add(k)
            if writable[i]:
                pw.add(k)
    n_swap, max_swap, n_jup, max_jup = 0, 0, 0, 0
    for ix in rec["instructions"]:
        prog, data = ix["program_id"], bytes(ix["data"])
        if prog in VENUES and len(data) >= 16 and data[:8] in SWAP_DISC:
            n_swap += 1
            max_swap = max(max_swap, u64(data, 8))
        elif prog == JUP6 and len(data) >= 27:
            n_jup += 1
            max_jup = max(max_jup, u64(data, len(data) - 19))
    return {"pools_static": sorted(ps), "pools_all": sorted(pa), "pools_writable": sorted(pw),
            "n_venue_swaps": n_swap, "max_venue_swap_amount": max_swap, "n_jup_routes": n_jup,
            "max_jup_in_amount": max_jup, "wsol_in_keys": WSOL in keys}


def masters(prep: Path) -> pl.DataFrame:
    m = json.loads((prep / "manifest.json").read_text())
    return pl.concat([pl.read_parquet(prep / "tx_labels" / n, columns=["master_tx_id", "signature"]) for n in m["labels"]["parts"]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--parallel", type=int, default=4, help="partitions processed at once")
    ap.add_argument("--downloads", type=int, default=6, help="concurrent raw part downloads per partition")
    ap.add_argument("--partitions", nargs="*")
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE.parent / "weekly"))
    if not (HERE.parent / "weekly" / "fleet").exists():  # local checkout: fleet/ lives in the labeller repo
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, fetch, published, raw_records

    store = Store(workers=16)
    plan = {p["id"]: p for p in json.loads(store.get(f"{RUN_PREFIX}/PLAN.json"))["partitions"]}
    ids = args.partitions or sorted(d.name for d in (args.week / "prep").iterdir())
    out = args.week / "extras"
    out.mkdir(exist_ok=True)

    # Pool universe: every partition's labelled-venue markets.
    pool_dir = args.week / "markets"
    for pid in plan:
        if not (pool_dir / pid / "markets.parquet").exists():
            m = published(store, f"{RUN_PREFIX}/index/{pid}")
            fetch(store, [r for r in m["files"] if r["path"] == "labels/markets.parquet"], pool_dir / pid, strip="labels/")
    mk = pl.concat([pl.read_parquet(pool_dir / pid / "markets.parquet", columns=["pool", "program"]) for pid in plan]).unique("pool")
    assert set(mk["program"].unique()) <= VENUES, mk["program"].unique()
    pools = set(mk["pool"].to_list())
    print(f"pool universe: {len(pools)} pools", flush=True)

    def one(pid: str) -> str:
        dest = out / f"{pid}.parquet"
        if dest.exists():
            return f"{pid}: exists"
        want = masters(args.week / "prep" / pid)
        recs = [r for r in raw_records(store, plan[pid]["source_prefix"]) if r["path"].endswith(".transactions.parquet")]
        rows = []
        with tempfile.TemporaryDirectory(dir=args.week) as tmp, ThreadPoolExecutor(args.downloads) as dl:
            def get(r):
                local = Path(tmp) / Path(r["path"]).name
                for attempt in range(5):   # object-store reads occasionally time out
                    try:
                        store.verify(r, local)
                        return local
                    except Exception:
                        local.unlink(missing_ok=True)
                        if attempt == 4:
                            raise
                        time.sleep(2 ** attempt)
            # At most `downloads` parts are on disk ahead of decoding; each part is deleted once read.
            pending = [dl.submit(get, r) for r in recs[:args.downloads]]
            nxt = len(pending)
            while pending:
                local = pending.pop(0).result()
                if nxt < len(recs):
                    pending.append(dl.submit(get, recs[nxt]))
                    nxt += 1
                # Read from memory: a memory-mapped file cannot be deleted on Windows.
                t = pl.read_parquet(io.BytesIO(local.read_bytes()), columns=["signature", "record_cbor"]).join(want, on="signature", how="inner")
                local.unlink()
                for mid, blob in zip(t["master_tx_id"].to_list(), t["record_cbor"].to_list()):
                    rows.append({"master_tx_id": mid, **extract(cbor2.loads(blob), pools)})
        df = pl.DataFrame(rows, schema=SCHEMA).unique("master_tx_id").sort("master_tx_id")
        missing = want.height - df.height
        df.write_parquet(dest.with_suffix(".partial"))
        dest.with_suffix(".partial").rename(dest)
        return f"{pid}: {df.height} masters ({missing} not found in raw), {sum(r['size'] for r in recs) / 1e9:.1f} GB streamed"

    with ThreadPoolExecutor(args.parallel) as pool_exec:
        for msg in pool_exec.map(one, ids):
            print(msg, flush=True)
    print("done")


if __name__ == "__main__":
    main()
