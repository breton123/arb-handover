#!/usr/bin/env python3
"""Rank unknown pools from CORE-010 FRAMED flow. Off hot path. No send.

Prefers paper_orbit framed_pool / unknown_pool lines. If those are not
in the journal yet, resolves a bounded tail of framed sigs via batched
getTransaction (retries=1). Never 12×15s backoff.
"""
from __future__ import annotations

import json
import sys
import urllib.request
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "paper004"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
import paper004 as p4  # noqa: E402

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
OUT = Path("/home/louis/captures/paper_orbit/FRAMED_POOLS.json")
DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
PUMP = live.PUMP
MAX_RESOLVE = 240
BATCH = 20


def rpc_batch(calls: list[tuple[str, list]], retries: int = 1) -> list:
    body = json.dumps(
        [{"jsonrpc": "2.0", "id": i, "method": m, "params": p} for i, (m, p) in enumerate(calls)]
    ).encode()
    last = None
    for _ in range(max(1, retries)):
        d._pace()
        req = urllib.request.Request(
            d.rpc_url(), data=body, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=45) as r:
                obj = json.loads(r.read().decode())
            if isinstance(obj, dict):
                obj = [obj]
            by_id = {int(x.get("id") or 0): x for x in obj if isinstance(x, dict)}
            return [by_id.get(i, {}) for i in range(len(calls))]
        except Exception as e:
            last = e
    raise last


def keys_of(tx: dict) -> list[str]:
    msg = ((tx.get("transaction") or {}).get("message") or {})
    raw = msg.get("accountKeys") or []
    out = []
    for k in raw:
        if isinstance(k, str):
            out.append(k)
        elif isinstance(k, dict):
            out.append(str(k.get("pubkey") or ""))
    meta = tx.get("meta") or {}
    loaded = meta.get("loadedAddresses") or {}
    out.extend(loaded.get("writable") or [])
    out.extend(loaded.get("readonly") or [])
    return [x for x in out if x]


def pool_from_tx(tx: dict) -> tuple[str, str] | None:
    msg = ((tx.get("transaction") or {}).get("message") or {})
    keys = keys_of(tx)
    ixs = list(msg.get("instructions") or [])
    inner = (tx.get("meta") or {}).get("innerInstructions") or []
    for block in inner:
        ixs.extend(block.get("instructions") or [])
    for ix in ixs:
        pid = ix.get("programId") or ""
        if not pid and isinstance(ix.get("programIdIndex"), int):
            idx = ix["programIdIndex"]
            pid = keys[idx] if idx < len(keys) else ""
        accounts = ix.get("accounts") or []
        accs = []
        for a in accounts:
            if isinstance(a, int) and a < len(keys):
                accs.append(keys[a])
            elif isinstance(a, str):
                accs.append(a)
        if pid == DLMM and accs:
            return accs[0], "dlmm"
        if pid == PUMP and accs:
            return accs[0], "pump"
    return None


def collect_from_audit(path: Path) -> tuple[Counter[str], dict[str, str], dict[str, int], list[str]]:
    freq: Counter[str] = Counter()
    proto: dict[str, str] = {}
    last: dict[str, int] = {}
    framed_sigs: list[str] = []
    if not path.exists():
        return freq, proto, last, framed_sigs
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("{"):
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = rec.get("kind")
            if kind in ("framed_pool", "unknown_pool"):
                hx = rec.get("pool_hex") or ""
                if len(hx) < 64:
                    continue
                try:
                    pk = d._pk(bytes.fromhex(hx[:64]))
                except Exception:
                    continue
                freq[pk] += 1
                pr = int(rec.get("proto") or 0)
                proto[pk] = "dlmm" if pr == 1 else "pump" if pr == 2 else proto.get(pk, "")
                continue
            if kind == "frame" and rec.get("class") == "framed":
                hx = rec.get("sig_hex") or ""
                if hx:
                    framed_sigs.append(hx)
    return freq, proto, last, framed_sigs


def resolve_sigs(hexes: list[str], freq: Counter[str], proto: dict[str, str]) -> int:
    uniq = list(dict.fromkeys(hexes[-MAX_RESOLVE * 2 :]))[-MAX_RESOLVE:]
    added = 0
    for i in range(0, len(uniq), BATCH):
        chunk = uniq[i : i + BATCH]
        calls = []
        for hx in chunk:
            try:
                sig = p4.b58encode(bytes.fromhex(hx))
            except Exception:
                continue
            calls.append(
                (
                    "getTransaction",
                    [sig, {"encoding": "json", "maxSupportedTransactionVersion": 1}],
                )
            )
        if not calls:
            continue
        try:
            rows = rpc_batch(calls, retries=1)
        except Exception as ex:
            print(f"  resolve_fail {type(ex).__name__}", flush=True)
            continue
        for row in rows:
            tx = (row or {}).get("result")
            if not tx:
                continue
            hit = pool_from_tx(tx)
            if not hit:
                continue
            pk, kind = hit
            freq[pk] += 1
            proto[pk] = kind
            added += 1
    return added


def main() -> int:
    live.load_dotenv()
    freq, proto, last, framed = collect_from_audit(AUDIT)
    tagged = sum(freq.values())
    resolved = 0
    if tagged < 8 and framed:
        print(f"FRAMED-RANK  tagged={tagged} resolve last {min(len(framed), MAX_RESOLVE)}", flush=True)
        resolved = resolve_sigs(framed, freq, proto)
    rows = [
        {"pool": pk, "n": n, "proto": proto.get(pk, ""), "last_seen": last.get(pk, 0)}
        for pk, n in freq.most_common()
    ]
    OUT.write_text(
        json.dumps({"n": len(rows), "tagged": tagged, "resolved": resolved, "rows": rows[:800]}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    print(
        f"FRAMED-RANK  pools={len(rows)} tagged={tagged} resolved={resolved} "
        f"top={[(r['pool'][:8], r['n'], r['proto']) for r in rows[:8]]}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
