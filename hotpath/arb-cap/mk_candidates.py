#!/usr/bin/env python3
"""Emit live002 --candidates from an existing liveuniv.json pool list."""
from __future__ import annotations

import json
import sys
from pathlib import Path

PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
CLMM = "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK"
CPMM = "CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C"
DAMM = "cpamdpZCGKUy5JxQXB4dcpGPiikHawvSWAd6mEn1sGG"
ORCA = "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc"
KIND = {
    "pump": PUMP,
    "dlmm": DLMM,
    "clmm": CLMM,
    "cpmm": CPMM,
    "damm": DAMM,
    "orca": ORCA,
}


def main() -> int:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "live002/liveuniv.json")
    dst = Path(sys.argv[2] if len(sys.argv) > 2 else "live002/candidates.json")
    blob = json.loads(src.read_text(encoding="utf-8"))
    out = {}
    for p in blob.get("pools", []):
        pid = KIND.get(p.get("kind", ""))
        pk = p.get("pubkey")
        if not pid or not pk:
            continue
        out[pk] = {"pid": pid, "usd": float(p.get("usd") or 0)}
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"candidates={len(out)} -> {dst}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
