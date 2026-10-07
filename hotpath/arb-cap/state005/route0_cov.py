#!/usr/bin/env python3
"""route0_addressable_coverage using the denomination contract.

A unique DLMM N is addressable iff its pool has
  DLMM.X == TOKEN != WSOL
  DLMM.Y == WSOL
and a Pump exists with
  Pump.base == TOKEN
  Pump.quote == WSOL

Unclassified pools are excluded from the denominator.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

SEEN = Path("/home/louis/captures/state005/seen_n.jsonl")
BIN = Path("/home/louis/captures/paper_orbit/liveuniv.bin")
OUT = Path("/home/louis/captures/paper_orbit/ROUTE0_COV.json")
CLASS = Path("/home/louis/captures/paper_orbit/POOL_CLASS.json")
RANKED = Path("/home/louis/captures/paper_orbit/UNKNOWN_RANKED.json")

INCOMPAT = {"dlmm_y_not_wsol", "stable_or_sol", "no_pump", "no_snap_or_not_sol"}


def load_bin(path: Path) -> list[dict]:
    b = path.read_bytes()
    magic, ver, n = struct.unpack_from("<IHH", b, 0)
    if magic != live.LIVE_MAGIC or n == 0:
        raise SystemExit("bad liveuniv")
    off = 24
    recs = []
    for _ in range(n):
        proto = b[off]
        pk = live.b58e(b[off + 1 : off + 33])
        mx = b[off + 33 : off + 65]
        my = b[off + 65 : off + 97]
        off += 161
        if proto == live.PROTO_DLMM:
            nbin = struct.unpack_from("<H", b, off + 71)[0]
            off += 73 + nbin * 20
        elif proto == live.PROTO_PUMP:
            off += 50
        else:
            raise SystemExit(f"proto {proto}")
        recs.append(
            {
                "proto": "dlmm" if proto == live.PROTO_DLMM else "pump",
                "pk": pk,
                "mx": mx,
                "my": my,
            }
        )
    return recs


def merge_class(recs: list[dict]) -> tuple[dict[str, str], set[str]]:
    cls: dict[str, str] = {}
    if CLASS.exists():
        try:
            raw = json.loads(CLASS.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                cls.update({str(k): str(v) for k, v in raw.items()})
        except json.JSONDecodeError:
            pass
    if RANKED.exists():
        try:
            rows = (json.loads(RANKED.read_text(encoding="utf-8")) or {}).get("rows") or []
            for row in rows:
                pk = row.get("pool")
                if not pk:
                    continue
                if row.get("partner"):
                    cls[pk] = "compatible"
                elif row.get("reason"):
                    cls[pk] = str(row["reason"])
        except json.JSONDecodeError:
            pass
    pumps = [p for p in recs if p["proto"] == "pump"]
    pump_tokens = {
        p["mx"]
        for p in pumps
        if live.b58e(p["my"]) == live.SOL and live.b58e(p["mx"]) != live.SOL
    }
    compiled: set[str] = set()
    for dlm in recs:
        if dlm["proto"] != "dlmm":
            continue
        if live.b58e(dlm["my"]) != live.SOL or live.b58e(dlm["mx"]) == live.SOL:
            cls[dlm["pk"]] = "dlmm_y_not_wsol"
            continue
        if dlm["mx"] in pump_tokens:
            cls[dlm["pk"]] = "compatible"
            compiled.add(dlm["pk"])
    return cls, compiled


def main() -> int:
    recs = load_bin(BIN)
    cls, compiled = merge_class(recs)
    all_n = addr = known = incompat = unknown = 0
    for line in SEEN.read_text(encoding="utf-8").splitlines() if SEEN.exists() else []:
        if not line.strip():
            continue
        row = json.loads(line)
        all_n += 1
        pk = d._pk(bytes.fromhex(row["pool"]))
        kind = cls.get(pk)
        if kind == "compatible":
            addr += 1
            if pk in compiled:
                known += 1
        elif kind in INCOMPAT:
            incompat += 1
        else:
            unknown += 1
    pct = (100.0 * known / addr) if addr else 0.0
    share = (100.0 * addr / all_n) if all_n else 0.0
    js = {
        "all_dlmm_n": all_n,
        "addressable": addr,
        "known_addressable": known,
        "incompatible": incompat,
        "unclassified": unknown,
        "n_dlmm_compiled_compatible": len(compiled),
        "route0_addressable_coverage_pct": pct,
        "addressable_share_of_all_n_pct": share,
        "note": (
            "denominator = unique DLMM N whose pool has X=TOKEN Y=WSOL "
            "and a Pump with base=TOKEN quote=WSOL. "
            "Unclassified pools are excluded from the denominator."
        ),
    }
    OUT.write_text(json.dumps(js, indent=2) + "\n")
    print(
        f"route0_addressable {known}/{addr}={pct:.1f}%  "
        f"share_of_all {addr}/{all_n}={share:.1f}%  "
        f"compiled_dlmm={len(compiled)} incompat={incompat} unclassified={unknown}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
