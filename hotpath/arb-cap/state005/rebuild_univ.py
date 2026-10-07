#!/usr/bin/env python3
"""Rebuild liveuniv.bin from current chain using bin-sum DLMM reserves. No send."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

from univ_gen_lock import acquire as acquire_gen_lock
from univ_gen_lock import release as release_gen_lock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402


def fetch_priced(pk: str) -> dict | None:
    sn = d.pricing_snap(pk, live.K)
    if sn is None or not sn.get("lb"):
        return None
    lb = sn["lb"]
    return {
        "snap": sn,
        "pubkey": live.b58d(pk),
        "mint_x": lb["token_x"],
        "mint_y": lb["token_y"],
        "vault_x": lb["vault_x"],
        "vault_y": lb["vault_y"],
        "slot": sn["slot"],
        "token": live.b58e(lb["token_x"])
        if live.b58e(lb["token_y"]) == live.SOL
        else live.b58e(lb["token_y"]),
        "sol_side": live.b58e(lb["token_x"]) == live.SOL
        or live.b58e(lb["token_y"]) == live.SOL,
    }


def main() -> int:
    acquire_gen_lock()
    try:
        return _main()
    finally:
        release_gen_lock()


def _main() -> int:
    live.load_dotenv()
    meta_path = Path(sys.argv[1])
    out = Path(sys.argv[2])
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    gcfg_pk = d._pk(d.find_pda([b"global_config"], live.b58d(live.PUMP)))
    gacc = d.get_multiple([gcfg_pk])[0]
    fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)
    ordered = []
    for p in meta.get("pools") or []:
        pk = p["pubkey"]
        kind = p.get("proto") or p.get("kind")
        if kind == "dlmm":
            r = fetch_priced(pk)
            if r:
                ordered.append(("dlmm", r))
                print(
                    f"  dlmm {p.get('idx')} {pk[:8]} active={r['snap']['lb']['active_id']} "
                    f"rx={r['snap']['reserve_x']} vault={r['snap'].get('vault_x')}",
                    flush=True,
                )
        elif kind == "pump":
            r = live.fetch_pump(pk, fees)
            if r:
                ordered.append(("pump", r))
                print(f"  pump {p.get('idx')} {pk[:8]}", flush=True)
    if not ordered:
        print("no pools", flush=True)
        return 1
    slot = max((r["slot"] for _, r in ordered), default=0)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        live.write_u32(f, live.LIVE_MAGIC)
        live.write_u16(f, live.LIVE_VER)
        live.write_u16(f, len(ordered))
        live.write_u64(f, slot)
        live.write_u64(f, slot if slot else 1)
        for kind, r in ordered:
            if kind == "dlmm":
                live.write_meta(
                    f, live.PROTO_DLMM, r["pubkey"], r["mint_x"], r["mint_y"],
                    r["vault_x"], r["vault_y"],
                )
                live.write_dlmm(f, r["snap"])
            else:
                live.write_meta(
                    f, live.PROTO_PUMP, r["pubkey"], r["mint_x"], r["mint_y"],
                    r["vault_x"], r["vault_y"],
                )
                live.write_pump(f, r)
    js = {
        "path": str(out),
        "n": len(ordered),
        "n_dlmm": sum(1 for k, _ in ordered if k == "dlmm"),
        "n_pump": sum(1 for k, _ in ordered if k == "pump"),
        "slot": slot,
        "reserve": "bin-sum",
        "pools": [
            {
                "idx": i,
                "proto": kind,
                "pubkey": live.b58e(r["pubkey"]),
                "sol_side": r["sol_side"],
            }
            for i, (kind, r) in enumerate(ordered)
        ],
    }
    (out.parent / "liveuniv.json").write_text(json.dumps(js, indent=2) + "\n")
    print(f"wrote {out} n={len(ordered)} slot={slot} reserve=bin-sum", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
