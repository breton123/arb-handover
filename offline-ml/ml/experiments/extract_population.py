"""Landing-head test data: every non-vote transaction that names a labelled-venue pool, landed or failed.

Streams each partition's raw `transactions.parquet` and `runtime.parquet` parts from Tigris. For transactions whose
resolved keys name a pool of the labelled venues, it writes pre-execution features (decoded from the raw
transaction only) and the execution outcome (from runtime metadata, used ONLY as the label):

    <week>/population/<partition>.parquet
        signature, slot, entry_index, tx_index, fee_payer
        label: landed_ok, error_kind, fee, cu_consumed            (runtime; never features)
        features: see FEATURES below                             (raw transaction only)

Population rule (approximation of the labeller's): a pool of Raydium CLMM / Orca Whirlpool / Meteora DLMM among the
ALT-resolved keys. Records are pre-filtered on the venue program ids appearing in the record bytes.

    python experiments/extract_population.py --week ... --credentials tigris.json --partitions ID ... [--parallel 4]
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
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import cbor2
import polars as pl

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from extract_extras import JUP6, RUN_PREFIX, SWAP_DISC, VENUES, WSOL, u64  # noqa: E402

VOTE = "Vote111111111111111111111111111111111111111"
COMPUTE_BUDGET = "ComputeBudget111111111111111111111111111111"
SYSTEM = "11111111111111111111111111111111"
JITO_TIPS = {
    "96gYZGLnJYVFmbjzopPSU6QiEV5fGqZNyN9nmNhvrZU5", "HFqU5x63VTqvQss8hp11i4wVV8bD44PvwucfZ2bU7gRe",
    "Cw8CFyM9FkoMi7K7Crf6HNQqf4uEMzpKw6QNghXLvLkY", "ADaUMid9yfUytqMBgopwjb2DTLSokTSzL1zt6iGPaS49",
    "DfXygSm4jCyNCybVYYK6DwvWqjKee8pbDmJGcLWNDXjh", "ADuUkR4vqLUMWXxW9gh6D6L8pMSawimctcNZ5pGwDcEt",
    "DttWaMuVvTiduZRnguLF7jNxTgiMBZ1hyAumKUiL2KRL", "3AVi9Tg9Uo68tJfuvoKvqKNWKkC5wPdSSdeBnizKZ6jT",
}
VENUE_BYTES = [v.encode() for v in VENUES]
VOTE_BYTES = VOTE.encode()


def h64(s: str) -> int:
    return int.from_bytes(hashlib.blake2b(s.encode(), digest_size=8).digest(), "little") >> 1


def wsol_cycle(pairs: list[tuple[str, str]]) -> bool:
    """True if the named pools' token pairs contain a cycle through WSOL (2+ pools)."""
    if len(pairs) < 2:
        return False
    adj = defaultdict(list)
    for i, (a, b) in enumerate(pairs):
        adj[a].append((b, i))
        adj[b].append((a, i))
    # A cycle through WSOL exists if two different WSOL edges reach each other without reusing an edge through WSOL.
    starts = [(nb, i) for nb, i in adj.get(WSOL, [])]
    for k, (n0, e0) in enumerate(starts):
        seen, stack = {n0}, [n0]
        while stack:
            n = stack.pop()
            for nb, i in adj[n]:
                if i == e0:
                    continue
                if nb == WSOL:
                    return True
                if nb not in seen:
                    seen.add(nb)
                    stack.append(nb)
    return False


def features(rec: dict, pools: dict) -> dict:
    keys = rec["resolved_keys"]
    static = rec["static_keys"]
    sset = set(static)
    writable = rec["writable"]
    signer = rec["signer"]
    header = rec["header"]
    ixs = rec["instructions"]
    named, named_static, named_writable = [], 0, 0
    for i, k in enumerate(keys):
        if k in pools:
            named.append(k)
            named_static += k in sset
            named_writable += bool(writable[i])
    progs = [ix["program_id"] for ix in ixs]
    n_swap = max_swap = n_jup = max_jup = tip = n_transfers = 0
    for ix in ixs:
        prog, data = ix["program_id"], bytes(ix["data"])
        if prog in VENUES and len(data) >= 16 and data[:8] in SWAP_DISC:
            n_swap += 1
            max_swap = max(max_swap, u64(data, 8))
        elif prog == JUP6 and len(data) >= 27:
            n_jup += 1
            max_jup = max(max_jup, u64(data, len(data) - 19))
        elif prog == SYSTEM and len(data) >= 12 and int.from_bytes(data[:4], "little") == 2:
            n_transfers += 1
            dest = keys[ix["accounts"][1]] if len(ix["accounts"]) > 1 else ""
            if dest in JITO_TIPS:
                tip += u64(data, 4)
    n_signers = header[0]
    cu_limit = rec.get("requested_cu_limit") or 0
    cu_price = rec.get("cu_price_micro_lamports") or 0
    payer = static[0]
    return {
        "fee_payer": payer,
        "version": 1 if str(rec.get("version")) not in ("legacy", "Legacy") else 0,
        "uses_alt": int(len(keys) > len(static)),
        "n_alt_tables": len(rec.get("alt_references") or []),
        "n_static_keys": len(static), "n_keys": len(keys), "n_signers": n_signers,
        "n_writable": int(sum(1 for w in writable if w)),
        "n_instructions": len(ixs), "n_distinct_programs": len(set(progs)),
        "data_bytes": sum(len(ix["data"]) for ix in ixs), "max_ix_data": max((len(ix["data"]) for ix in ixs), default=0),
        "raw_len": len(rec["raw"]),
        "prog0": h64(progs[0]) if progs else 0, "prog1": h64(progs[1]) if len(progs) > 1 else 0,
        "prog2": h64(progs[2]) if len(progs) > 2 else 0, "prog_last": h64(progs[-1]) if progs else 0,
        "prog_seq": h64("|".join(p for p in progs if p != COMPUTE_BUDGET)),
        "n_compute_budget_ix": progs.count(COMPUTE_BUDGET),
        "cu_limit": cu_limit, "cu_price": cu_price, "priority_fee": cu_limit * cu_price // 1_000_000,
        "jito_tip": tip, "has_tip": int(tip > 0), "n_transfers": n_transfers,
        "durable_nonce": int(rec.get("durable_nonce") is not None),
        "payer_only_signer": int(n_signers == 1),
        "n_pools": len(set(named)), "n_pools_static": named_static, "n_pools_writable": named_writable,
        "n_venue_swaps": n_swap, "max_venue_swap": max_swap, "n_jup_routes": n_jup, "max_jup_in": max_jup,
        "wsol_in_keys": int(WSOL in keys),
        "attempted_arb_static": int(wsol_cycle([pools[p] for p in set(named)])),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--week", required=True, type=Path)
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--partitions", nargs="+", required=True)
    ap.add_argument("--parallel", type=int, default=4)
    ap.add_argument("--downloads", type=int, default=6)
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE.parent / "weekly"))
    if not (HERE.parent / "weekly" / "fleet").exists():
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, published

    store = Store(workers=16)
    plan = {p["id"]: p for p in json.loads(store.get(f"{RUN_PREFIX}/PLAN.json"))["partitions"]}
    mk = pl.concat([pl.read_parquet(f, columns=["pool", "token_a", "token_b"]) for f in (args.week / "markets").glob("*/markets.parquet")]).unique("pool")
    pools = {r[0]: (r[1], r[2]) for r in mk.iter_rows()}
    out = args.week / "population"
    out.mkdir(exist_ok=True)
    print(f"pool universe {len(pools)}", flush=True)

    def one(pid: str) -> str:
        dest = out / f"{pid}.parquet"
        if dest.exists():
            return f"{pid}: exists"
        m = published(store, plan[pid]["source_prefix"])
        recs = {r["path"]: r for r in m["files"]}
        parts = sorted(p for p in recs if p.endswith(".transactions.parquet"))
        rows = []
        t0 = time.time()
        with tempfile.TemporaryDirectory(dir=args.week) as tmp, ThreadPoolExecutor(args.downloads) as dl:
            def get(path):
                pair = (path, path.replace(".transactions.", ".runtime."))
                data = []
                for pth in pair:
                    local = Path(tmp) / Path(pth).name
                    for attempt in range(5):
                        try:
                            store.verify(recs[pth], local)
                            break
                        except Exception:
                            local.unlink(missing_ok=True)
                            if attempt == 4:
                                raise
                            time.sleep(2 ** attempt)
                    data.append(local.read_bytes())
                    local.unlink()
                return data
            pending = [dl.submit(get, p) for p in parts[:args.downloads]]
            nxt = len(pending)
            while pending:
                txb, rtb = pending.pop(0).result()
                if nxt < len(parts):
                    pending.append(dl.submit(get, parts[nxt]))
                    nxt += 1
                t = pl.read_parquet(io.BytesIO(txb), columns=["slot", "entry_index", "tx_index", "signature", "record_cbor"])
                keep = []
                for i, blob in enumerate(t["record_cbor"].to_list()):
                    if VOTE_BYTES in blob or not any(v in blob for v in VENUE_BYTES):
                        continue
                    rec = cbor2.loads(blob)
                    if not any(k in pools for k in rec["resolved_keys"]):
                        continue
                    keep.append((i, rec))
                if not keep:
                    continue
                sigs = {t["signature"][i] for i, _ in keep}
                rt = pl.read_parquet(io.BytesIO(rtb), columns=["signature", "record_cbor"]).filter(pl.col("signature").is_in(list(sigs)))
                outcome = {}
                for sig, blob in zip(rt["signature"].to_list(), rt["record_cbor"].to_list()):
                    md = cbor2.loads(blob)["Event"]["event"]["metadata"]
                    err = md.get("error")
                    outcome[sig] = (bool(md.get("success")), (next(iter(err)) if isinstance(err, dict) else (str(err) if err else "")),
                                    md.get("fee") or 0, md.get("cu_consumed") or 0)
                for i, rec in keep:
                    sig = t["signature"][i]
                    if sig not in outcome:
                        continue
                    ok, ek, fee, cu = outcome[sig]
                    rows.append({"signature": sig, "slot": t["slot"][i], "entry_index": t["entry_index"][i], "tx_index": t["tx_index"][i],
                                 "landed_ok": ok, "error_kind": ek, "fee": fee, "cu_consumed": cu, **features(rec, pools)})
        df = pl.DataFrame(rows, infer_schema_length=None).sort("slot", "entry_index", "tx_index")
        df.write_parquet(dest.with_suffix(".partial"))
        dest.with_suffix(".partial").rename(dest)
        return f"{pid}: {df.height} txs, landed_ok {df['landed_ok'].mean():.3f}, {time.time() - t0:.0f}s"

    with ThreadPoolExecutor(args.parallel) as ex:
        for msg in ex.map(one, args.partitions):
            print(msg, flush=True)
    print("done")


if __name__ == "__main__":
    main()
