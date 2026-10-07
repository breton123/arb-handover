#!/usr/bin/env python3
"""Compile ARBHOPS0 templates for every closed route. Does not publish ALTs.

Writes a separate plane. Never overwrites oneshot alt_plane.json.
No opportunity-time provisioning. No send.
"""
from __future__ import annotations

import json
import os
import struct
from collections import defaultdict
from pathlib import Path

DISC = b"ARBHOPS0"
SOL = "So11111111111111111111111111111111111111112"
DUMP = Path("/home/louis/arb-cap/fam6/ROUTES.json")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT = Path("/home/louis/arb-cap/fam6/hops_plane.json")
ONESHOT_PLANE = Path("/home/louis/arb-exec/.deploy/alt_plane.json")
ALT_MAX = 256


def pack_ix(hops: list[tuple[int, int, int, int]]) -> str:
    buf = bytearray(40)
    buf[:8] = DISC
    buf[8] = len(hops)
    struct.pack_into("<QQ", buf, 9, 50_000_000, 1_000_000_000)
    for i, (proto, ina, outa, n) in enumerate(hops):
        buf[25 + i * 3] = proto
        buf[26 + i * 3] = ina
        buf[27 + i * 3] = outa
        buf[34 + i] = n
    return buf.hex()


def hop_n(proto: str, in_ata: int) -> int:
    if proto == "dlmm":
        return 10
    return 26 if in_ata == 0 else 24


def main() -> int:
    routes = json.loads(DUMP.read_text(encoding="utf-8"))
    pools = (json.loads(UNIV.read_text(encoding="utf-8")).get("pools") or [])
    compiled = []
    keys: set[str] = set()
    for r in routes.get("routes") or []:
        if not r.get("exec"):
            continue
        seq = r["seq"].split("-")
        n = int(r["n_hop"])
        hops = []
        mints = [SOL]
        idxs = [r.get("p0"), r.get("p1"), r.get("p2")][:n]
        ata_n = 1
        for hi, proto in enumerate(seq):
            p = pools[int(idxs[hi])]
            mx = str(p.get("mx") or p.get("mint_x") or "")
            my = str(p.get("my") or p.get("mint_y") or "")
            incoming = mints[-1]
            outgoing = my if incoming == mx else mx
            mints.append(outgoing)
            ina = 0 if incoming == SOL else (1 if incoming == mints[1] or hi == 0 else min(hi, 2))
            if incoming == SOL:
                ina = 0
            elif incoming == mints[1]:
                ina = 1
            else:
                ina = 2
            outa = 0 if outgoing == SOL else (1 if outgoing == mints[1] else 2)
            if outgoing != SOL:
                ata_n = max(ata_n, outa + 1)
            hops.append((1 if proto == "dlmm" else 2, ina, outa, hop_n(proto, ina)))
            pk = p.get("pubkey")
            if pk:
                keys.add(pk)
        compiled.append({
            "id": r["id"],
            "fam": r["fam"],
            "seq": r["seq"],
            "n_hop": n,
            "ix": pack_ix(hops),
            "hops": hops,
            "mints": mints,
            "template": 1,
            "ata_ready": 0,
            "alt_ready": 0,
            "race_ready": 0,
        })
    prev = {}
    if OUT.exists():
        for old in (json.loads(OUT.read_text(encoding="utf-8")).get("routes") or []):
            prev[old.get("id")] = old
    kept = 0
    for row in compiled:
        old = prev.get(row["id"])
        if not old:
            continue
        for k in ("ata_ready", "alt_ready", "size_ok", "race_ready"):
            if old.get(k):
                row[k] = old[k]
        if row.get("race_ready"):
            kept += 1
    # Dedup ALT packing plan only. Do not create on-chain ALTs here.
    addrs = sorted(keys)
    tables = [addrs[i:i + ALT_MAX] for i in range(0, max(len(addrs), 1), ALT_MAX)]
    oneshot = ONESHOT_PLANE.exists()
    out = {
        "program": "ARBHOPS0",
        "oneshot_plane_untouched": True,
        "oneshot_plane_exists": oneshot,
        "n_template": len(compiled),
        "unique_pools": len(keys),
        "alt_tables_planned": len(tables),
        "alt_published": 0,
        "ata_created": 0,
        "race_ready": kept,
        "note": "new templates compile; existing RACE_READY rows stay marked",
        "routes": compiled,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(out) + "\n", encoding="utf-8")
    print(
        f"HOPS_PLANE  templates={len(compiled)} pools={len(keys)} "
        f"alt_tables_planned={len(tables)} published=0 RACE_READY={kept} "
        f"kept_ready={kept} oneshot_plane_untouched=1",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
