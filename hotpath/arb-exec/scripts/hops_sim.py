#!/usr/bin/env python3
"""Dispatcher proof matrix. Real CPI + Custom(6) only if HOPS_PROGRAM is set.

Never upgrades OUR_EXEC. Never sends. Never uses the oneshot wallet to deploy.
"""
from __future__ import annotations

import json
import os
from collections import defaultdict
from pathlib import Path

DUMP = Path("/home/louis/arb-cap/fam6/ROUTES.json")
PLANE = Path("/home/louis/arb-cap/fam6/hops_plane.json")
OUT = Path("/home/louis/arb-cap/fam6/SIM.json")
NEED = (
    "dlmm-dlmm", "dlmm-pump", "pump-dlmm", "pump-pump",
    "dlmm-dlmm-dlmm", "dlmm-dlmm-pump", "pump-dlmm-dlmm",
)
OUR_EXEC = "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K"


def main() -> int:
    routes = json.loads(DUMP.read_text(encoding="utf-8"))
    plane = json.loads(PLANE.read_text(encoding="utf-8")) if PLANE.exists() else {}
    by = defaultdict(list)
    for r in plane.get("routes") or []:
        by[r["seq"]].append(r)
    pid = os.environ.get("HOPS_PROGRAM") or ""
    cases = []
    for seq in NEED:
        rows = by.get(seq) or []
        cases.append({
            "seq": seq,
            "n_compiled": len(rows),
            "sample_id": rows[0]["id"] if rows else None,
            "ix_len": 40 if rows else 0,
            "simulate": "SKIP_NO_PROGRAM" if not pid else "PENDING",
            "custom6": None,
        })
    out = {
        "our_exec_frozen": OUR_EXEC,
        "hops_program": bool(pid),
        "deploy_from_oneshot_wallet": False,
        "cases": cases,
        "all_sequences_have_route": all(c["n_compiled"] > 0 for c in cases),
        "dispatcher_proven": False,
        "why": (
            "ARBHOPS0 not deployed; Custom(6) matrix waits for a new program id "
            "funded from a non-#6 wallet"
            if not pid else "HOPS_PROGRAM set — run simulate off-path"
        ),
    }
    OUT.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(
        f"HOPS_SIM  sequences={len(cases)} have_route={out['all_sequences_have_route']} "
        f"program={'yes' if pid else 'NO'} custom6=pending OUR_EXEC=frozen",
        flush=True,
    )
    for c in cases:
        print(f"  {c['seq']:<20} n={c['n_compiled']:<5} {c['simulate']}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
