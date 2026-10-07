#!/usr/bin/env python3
"""Why did high-freq unknown DLMM fail pricing_snap?"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

POOLS = [
    "EXXfbVutbuexDZtPw8pkSvWxLFoUMBHd544CAGkmtPZm",
    "GeUkx21Vc6yg63YZ1BdXY95ZATm9LeBYCgSN1uJ3o18S",
    "DdMA1cHcHEqYfttc1z1sJEY978CcU1pyjNuTWTNmdvzU",
    "6qz7THwQvcjF3HyDGLuKaLBUk6EyJKeZXZMWLAeiwfjd",
    "9Ux4vtd8juEH4NMF4ae3tXYpKBdeCUUipX8z3EficKme",
    "FhdW3Y6Ea6hXKbkGkt5YSAVtDNd5qJ8USevMaPyEr45S",
]


def main() -> int:
    live.load_dotenv()
    for pk in POOLS:
        try:
            sn = d.pricing_snap(pk, live.K)
        except Exception as e:
            print(f"{pk[:8]} EXC {type(e).__name__} {e}", flush=True)
            continue
        if sn is None:
            print(f"{pk[:8]} snap=None", flush=True)
            continue
        lb = sn.get("lb") or {}
        tx = d._pk(lb.get("token_x") or b"\x00" * 32) if lb.get("token_x") else ""
        ty = d._pk(lb.get("token_y") or b"\x00" * 32) if lb.get("token_y") else ""
        print(
            f"{pk[:8]} ok active={lb.get('active_id')} bins={len(sn.get('bins') or [])} "
            f"rx={sn.get('reserve_x')} ry={sn.get('reserve_y')} "
            f"x={tx[:8]} y={ty[:8]}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
