#!/usr/bin/env python3
"""One-shot preload of metadata-aware ALT2 cache. Off-path only. FUNDED=0."""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
from alt_plane import EXEC_PLANE, ONESHOT_DLMM, fetch_one, seed_oneshot_due  # noqa: E402
from alt_rpc import merge_record, write_cache  # noqa: E402
from b58 import b58decode  # noqa: E402
import rpc_sched  # noqa: E402

DIR = Path(os.environ.get("TRIGGER011_DIR", str(Path.home() / "captures" / "trigger011")))
JSON_OUT = DIR / "alt_cache.json"
BIN_OUT = DIR / "alt_cache.bin"
MISS = DIR / "alt_miss.jsonl"


def miss_keys(limit: int = 64) -> list[str]:
    from b58 import b58encode
    out = []
    seen = set()
    if not MISS.exists():
        return out
    for line in MISS.read_text(encoding="utf-8", errors="replace").splitlines()[-4000:]:
        try:
            hx = json.loads(line).get("alt_hex", "")
        except (ValueError, TypeError):
            continue
        if len(hx) != 64 or hx in seen:
            continue
        seen.add(hx)
        out.append(b58encode(bytes.fromhex(hx)))
        if len(out) >= limit:
            break
    return out


def main() -> int:
    DIR.mkdir(parents=True, exist_ok=True)
    url = rpc_sched.rpc_url()
    cache = json.loads(JSON_OUT.read_text(encoding="utf-8")) if JSON_OUT.exists() else {}
    due: dict[str, float] = {}
    seeded = seed_oneshot_due(due)
    for pk in miss_keys():
        due.setdefault(pk, 0.0)
    ok = bad = 0
    for pk in list(due):
        try:
            rec = fetch_one(url, pk)
        except Exception as ex:
            print(f"WARM  fail {pk[:8]} {type(ex).__name__}", flush=True)
            bad += 1
            continue
        if rec is None:
            cache.pop(pk, None)
        else:
            cache[pk] = merge_record(cache.get(pk), rec)
        ok += 1
        time.sleep(0.05)
    clean = {}
    for pk, rec in cache.items():
        if not isinstance(rec, dict) or not rec.get("addresses"):
            continue
        try:
            if len(b58decode(pk)) != 32:
                continue
            for addr in rec["addresses"]:
                if len(b58decode(addr)) != 32:
                    raise ValueError("addr")
            if rec.get("observed_bank_hash"):
                if len(b58decode(rec["observed_bank_hash"])) != 32:
                    rec = dict(rec)
                    rec["observed_bank_hash"] = None
            clean[pk] = rec
        except Exception:
            continue
    cache = clean

    def b32(value):
        raw = value if isinstance(value, (bytes, bytearray)) else b58decode(value)
        if len(raw) > 32:
            raise ValueError("Invalid 32-byte ALT key")
        return bytes(raw).rjust(32, b"\x00")

    write_cache(cache, JSON_OUT, BIN_OUT, b32)
    oneshot_alts = [k for k in due if k in cache]
    print(
        json.dumps({
            "dir": str(DIR),
            "plane": str(EXEC_PLANE),
            "oneshot_seed": seeded,
            "cache": len(cache),
            "fetched_ok": ok,
            "fetched_fail": bad,
            "bin": str(BIN_OUT),
            "bin_bytes": BIN_OUT.stat().st_size if BIN_OUT.exists() else 0,
            "oneshot_dlmm": sorted(ONESHOT_DLMM),
            "warmed": oneshot_alts,
        }, indent=2),
        flush=True,
    )
    return 0 if ok and bad == 0 else (0 if ok else 1)


if __name__ == "__main__":
    raise SystemExit(main())
