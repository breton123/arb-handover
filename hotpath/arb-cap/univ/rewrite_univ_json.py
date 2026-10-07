#!/usr/bin/env python3
"""Rebuild liveuniv.json mints from liveuniv.bin. No RPC. No popularity prune.

The hops vector builder reads mx/my from this json. Ingest must not
strip them — a mint-less json makes every ATA/direction wrong.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402

UNIV_BIN = Path("/home/louis/captures/paper_orbit/liveuniv.bin")
UNIV_JS = Path("/home/louis/captures/paper_orbit/liveuniv.json")


def pools_from_bin(path: Path) -> tuple[list[dict], int]:
    b = path.read_bytes()
    magic, ver, n = struct.unpack_from("<IHH", b, 0)
    if magic != live.LIVE_MAGIC or ver not in (live.LIVE_VER, 2) or n == 0:
        raise SystemExit(f"bad liveuniv {path}")
    slot = struct.unpack_from("<Q", b, 8)[0]
    off = 24
    pools = []
    for i in range(n):
        proto = b[off]
        pk = live.b58e(b[off + 1 : off + 33])
        mx = live.b58e(b[off + 33 : off + 65])
        my = live.b58e(b[off + 65 : off + 97])
        off += 161
        if proto == live.PROTO_DLMM:
            nbin = struct.unpack_from("<H", b, off + 71)[0]
            off += 73 + nbin * 20
        elif proto == live.PROTO_PUMP:
            off += 50
        else:
            raise SystemExit(f"unsupported proto {proto}")
        kind = "dlmm" if proto == live.PROTO_DLMM else "pump"
        token = mx if my == live.SOL else my
        pools.append({
            "idx": i,
            "proto": kind,
            "kind": kind,
            "pubkey": pk,
            "mx": mx,
            "my": my,
            "mint_x": mx,
            "mint_y": my,
            "token": token,
            "sol_side": mx == live.SOL or my == live.SOL,
        })
    return pools, slot


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else UNIV_BIN
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else UNIV_JS
    prev = {}
    if dst.exists():
        try:
            prev = json.loads(dst.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            prev = {}
    pools, slot = pools_from_bin(src)
    missing = sum(1 for p in pools if not p["mx"] or not p["my"])
    if missing:
        raise SystemExit(f"bin missing mints n={missing}")
    out = {
        **{k: prev[k] for k in prev if k != "pools"},
        "path": str(src),
        "n": len(pools),
        "n_dlmm": sum(1 for p in pools if p["proto"] == "dlmm"),
        "n_pump": sum(1 for p in pools if p["proto"] == "pump"),
        "slot": slot,
        "mints_from_bin": True,
        "pools": pools,
    }
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(dst.suffix + ".tmp")
    tmp.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    tmp.replace(dst)
    print(
        f"UNIV-JSON  n={out['n']} dlmm={out['n_dlmm']} pump={out['n_pump']} "
        f"mints_from_bin=1 missing={missing}",
        flush=True,
    )
    print(f"UNIV-JSON  wrote {dst}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
