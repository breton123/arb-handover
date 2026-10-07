#!/usr/bin/env python3
"""Live ALT control plane. Off-path only.

Tails ~/captures/trigger011/alt_miss.jsonl, fetches lookup tables,
writes alt_cache.bin for paper_orbit to reload. Never called from the
shred loop.
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

sys_path = Path(__file__).resolve().parent
import sys
sys.path.insert(0, str(sys_path))
sys.path.insert(0, str(sys_path.parent))
from b58 import b58decode, b58encode
from alt_rpc import fetch_record, merge_record, write_cache
import rpc_sched  # noqa: E402

DIR = Path(os.environ.get("TRIGGER011_DIR", str(Path.home() / "captures" / "trigger011")))
MISS = DIR / "alt_miss.jsonl"
JSON_OUT = DIR / "alt_cache.json"
BIN_OUT = DIR / "alt_cache.bin"
MET = DIR / "alt_plane.json"
EXEC_PLANE = Path(os.environ.get(
    "EXEC_ALT_PLANE", str(Path.home() / "arb-exec" / ".deploy" / "alt_plane.json")))
ONESHOT_DLMM = frozenset({
    "9xiLuqDSvN1pgyHSHCeRbrKFTe1DWbPAMJRm2usYgdaX",
    "8eLx26sto5xBSg8r1ksidtkmQyHzV2JUrQU97qmKw1Tq",
    "E27r15wB77JfF1hFkfeqoP2ZyZHKMWKgZ9Aix2cdPAuq",
})


def seed_oneshot_due(due: dict) -> int:
    n = 0
    if not EXEC_PLANE.exists():
        return 0
    try:
        doc = json.loads(EXEC_PLANE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 0
    for route in doc.get("routes") or []:
        if not isinstance(route, dict) or route.get("dlmm") not in ONESHOT_DLMM:
            continue
        keys = [route.get("alt"), route.get("alt0"), route.get("alt1")]
        keys.extend(route.get("alts") or [])
        for hop in (route.get("hops") or []):
            if isinstance(hop, dict):
                keys.append(hop.get("alt"))
                keys.extend(hop.get("alts") or [])
        for pk in keys:
            if isinstance(pk, str) and 32 <= len(pk) <= 48:
                due.setdefault(pk, 0.0)
                n += 1
    return n


def rpc(_url: str, method: str, params):
    return rpc_sched.call(method, params, channel="RPC_ALT_WARM")


def fetch_one(url: str, pk: str) -> dict | None:
    return fetch_record(rpc, url, pk, b58encode)


def refresh_due(cache: dict, due: dict, attempts: dict, url: str, now: float,
                fetcher=fetch_one) -> tuple[bool, int, int]:
    changed, success, failed = False, 0, 0
    for pk in sorted(due, key=due.get)[:8]:
        if due[pk] > now:
            break
        try:
            record = fetcher(url, pk)
        except Exception:
            attempts[pk] = attempts.get(pk, 0) + 1
            due[pk] = now + min(60, 2 ** min(attempts[pk], 6))
            failed += 1
            continue
        attempts.pop(pk, None)
        due[pk] = now + 30
        if record is None:
            if not (isinstance(cache.get(pk), dict) and cache[pk].get("quarantined")):
                changed |= cache.pop(pk, None) is not None
        else:
            cache[pk] = merge_record(cache.get(pk), record)
            changed = True
        success += 1
    return changed, success, failed


def main() -> int:
    DIR.mkdir(parents=True, exist_ok=True)
    url = rpc_sched.rpc_url()
    cache = {}
    if JSON_OUT.exists():
        cache = json.loads(JSON_OUT.read_text(encoding="utf-8"))
    due = dict.fromkeys(cache, 0.0)  # Old list-only caches are refetched.
    seeded = seed_oneshot_due(due)
    attempts = {}
    pos = MISS.stat().st_size if MISS.exists() else 0
    fetches = 0
    hits = 0
    fails = 0
    print(
        f"ALT-PLANE  dir={DIR} cached={len(cache)} oneshot_seed={seeded} "
        f"rpc={rpc_sched.rpc_host()} FUNDED=0",
        flush=True,
    )
    while True:
        if MISS.exists():
            if MISS.stat().st_size < pos:
                pos = 0
            with MISS.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(pos)
                for _ in range(1000):
                    line = f.readline()
                    if not line.endswith("\n"):
                        break  # Retry incomplete records on the next poll.
                    pos = f.tell()
                    try:
                        hx = json.loads(line).get("alt_hex", "")
                        if len(hx) == 64:
                            due.setdefault(b58encode(bytes.fromhex(hx)), 0.0)
                    except (ValueError, TypeError, AttributeError):
                        continue
        changed, ok, bad = refresh_due(cache, due, attempts, url, time.monotonic())
        fetches += ok + bad
        hits += ok
        fails += bad
        if changed:
            write_cache(cache, JSON_OUT, BIN_OUT, b58decode)
        if ok or bad:
            MET.write_text(json.dumps({
                "cache": len(cache),
                "fetches": fetches,
                "hits": hits,
                "fails": fails,
                "unique_miss": len(due),
                "retrying": len(attempts),
                "quarantined": sum(isinstance(r, dict) and r.get("quarantined", False) for r in cache.values()),
            }), encoding="utf-8")
            print(f"ALT-PLANE  cache={len(cache)} fetch={fetches} fail={fails}", flush=True)
        time.sleep(0.5)


if __name__ == "__main__":
    raise SystemExit(main())
