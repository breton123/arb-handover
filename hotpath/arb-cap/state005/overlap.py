#!/usr/bin/env python3
"""Compare decoded STATE-005 pool keys against liveuniv.bin membership."""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import record_dlmm as d  # noqa: E402

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58e(raw: bytes) -> str:
    n = int.from_bytes(raw, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return "1" * pad + (out or "1")


def main() -> int:
    seen = Path(sys.argv[1])
    univ = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    want = {p["pubkey"] for p in univ.get("pools") or []}
    print(f"univ n={len(want)} reserve={univ.get('reserve')}")
    n = 0
    known = 0
    hits = Counter()
    miss = Counter()
    for line in seen.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        n += 1
        pk = d._pk(bytes.fromhex(row["pool"]))
        if pk in want:
            known += 1
            hits[pk[:8]] += 1
        else:
            miss[pk[:8]] += 1
    print(f"decoded={n} known={known} unknown={n - known}")
    print("hit", hits.most_common(8))
    print("miss", miss.most_common(10))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
