#!/usr/bin/env python3
"""PAPER run snapshot. Read-only."""
from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
PEND = Path("/home/louis/captures/paper_orbit/pending.jsonl")
AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
COV = Path("/home/louis/captures/paper_orbit/ROUTE0_COV.json")

u = json.loads(UNIV.read_text(encoding="utf-8"))
pools = u.get("pools") or []
print(f"univ gen={u.get('gen')} n={u.get('n')} dlmm={u.get('n_dlmm')} pump={u.get('n_pump')}")
print(
    "HTvj",
    sum(1 for p in pools if str(p.get("pubkey") or "").startswith("HTvjzsfX")),
    "Gf7s",
    sum(1 for p in pools if str(p.get("pubkey") or "").startswith("Gf7sXMoP")),
)

idxs = Counter()
for line in PEND.read_text(encoding="utf-8").splitlines() if PEND.exists() else []:
    if not line.strip():
        continue
    r = json.loads(line)
    idx = r.get("pool_idx")
    if idx is None and isinstance(r.get("n"), dict):
        idx = r["n"].get("pool_idx")
    idxs[idx] += 1
print("pending", sum(idxs.values()), "by_idx", idxs.most_common(10))
for idx, n in idxs.most_common(8):
    if idx is None or not isinstance(idx, int) or idx >= len(pools):
        print(f"  idx={idx} n={n} ?")
        continue
    p = pools[idx]
    print(f"  idx={idx} n={n} {p.get('proto')} {str(p.get('pubkey') or '')[:8]} tok={str(p.get('token') or '')[:8]}")

audit_n = 0
if AUDIT.exists():
    audit_n = sum(1 for x in AUDIT.read_text(encoding="utf-8").splitlines() if x.strip())
print("opp_synced_audit", audit_n)
if COV.exists():
    print("cov", json.loads(COV.read_text(encoding="utf-8")))
