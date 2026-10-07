#!/usr/bin/env python3
"""Complete {DLMM,Pump} route-closure inventory. Observe only. No send."""
from __future__ import annotations

import json
import os
import struct
from collections import Counter, defaultdict
from pathlib import Path

SOL = "So11111111111111111111111111111111111111112"
PROTO = {"dlmm": 1, "pump": 2}
SEQS_2 = ("dlmm-dlmm", "dlmm-pump", "pump-dlmm", "pump-pump")
SEQS_3 = (
    "dlmm-dlmm-dlmm", "dlmm-dlmm-pump", "dlmm-pump-dlmm", "dlmm-pump-pump",
    "pump-dlmm-dlmm", "pump-dlmm-pump", "pump-pump-dlmm", "pump-pump-pump",
)
DISC = b"ARBHOPS0"
UNIV = Path(os.environ.get("LIVEUNIV", "/home/louis/captures/paper_orbit/liveuniv.json"))
OUT = Path("/home/louis/arb-cap/fam6/CLOSURE.json")


def other(mx: str, my: str, incoming: str) -> str | None:
    if incoming == mx:
        return my
    if incoming == my:
        return mx
    return None


def pack_ix(hops: list[tuple[int, int, int, int]], amount: int, min_profit: int) -> bytes:
    """hops: (proto, in_ata, out_ata, acc_n) x 2 or 3."""
    buf = bytearray(40)
    buf[:8] = DISC
    buf[8] = len(hops)
    struct.pack_into("<QQ", buf, 9, amount, min_profit)
    for i, (proto, ina, outa, n) in enumerate(hops):
        buf[25 + i * 3] = proto
        buf[26 + i * 3] = ina
        buf[27 + i * 3] = outa
        buf[34 + i] = n
    return bytes(buf)


def main() -> int:
    u = json.loads(UNIV.read_text(encoding="utf-8"))
    pools = u.get("pools") or []
    edges = []
    for i, p in enumerate(pools):
        proto = str(p.get("proto") or p.get("kind") or "")
        if proto not in PROTO:
            continue
        mx = str(p.get("mx") or p.get("mint_x") or "")
        my = str(p.get("my") or p.get("mint_y") or "")
        if not mx or not my or mx == my:
            continue
        edges.append((i, proto, mx, my, p.get("pubkey")))

    by_mint: dict[str, list[int]] = defaultdict(list)
    for ei, (_, _, mx, my, _) in enumerate(edges):
        by_mint[mx].append(ei)
        by_mint[my].append(ei)

    two = []
    three = []
    for e1i, (i1, p1, mx1, my1, pk1) in enumerate(edges):
        for incoming, first_out in ((SOL, other(mx1, my1, SOL)),):
            if not first_out or first_out == SOL:
                continue
            for e2i in by_mint[first_out]:
                if e2i == e1i:
                    continue
                i2, p2, mx2, my2, pk2 = edges[e2i]
                mid = other(mx2, my2, first_out)
                if mid is None:
                    continue
                if mid == SOL:
                    two.append((p1, p2, i1, i2, first_out))
                    continue
                if mid == first_out:
                    continue
                for e3i in by_mint[mid]:
                    if e3i in (e1i, e2i):
                        continue
                    i3, p3, mx3, my3, pk3 = edges[e3i]
                    back = other(mx3, my3, mid)
                    if back == SOL:
                        three.append((p1, p2, p3, i1, i2, i3, first_out, mid))

    c2 = Counter(f"{a}-{b}" for a, b, *_ in two)
    c3 = Counter(f"{a}-{b}-{c}" for a, b, c, *_ in three)
    seq = {s: c2.get(s, 0) for s in SEQS_2}
    seq.update({s: c3.get(s, 0) for s in SEQS_3})
    missing = [s for s, n in seq.items() if n == 0]
    ix = pack_ix([(1, 0, 1, 10), (1, 1, 0, 10)], 50_000_000, 1_000_000_000)
    assert len(ix) == 40 and ix[:8] == DISC and ix[8] == 2

    report = {
        "invariant": "supported venues => complete executable route closure",
        "venues": ["dlmm", "pump"],
        "n_pool_supported": len(edges),
        "closed_2": len(two),
        "closed_3": len(three),
        "seq": seq,
        "graph_missing": missing,
        "note_missing": "0 means the typed mint graph has no such close, not an executor hole",
        "family_stamp": "0=route0 5=typed-inverted 6=other supported 255=unsupported venue",
        "executor": "ARBHOPS0 program_hops  hop_count=2|3  proto=DLMM|PUMP",
        "our_exec": "38dsYLgt frozen — not upgraded",
        "ix_len": len(ix),
        "oneshot6": "untouched",
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(
        f"CLOSURE  pools={len(edges)} 2hop={len(two)} 3hop={len(three)} "
        f"missing_graph={missing}",
        flush=True,
    )
    for s in SEQS_2 + SEQS_3:
        print(f"  {s:<20} {seq[s]:6}", flush=True)
    print("  ix=ARBHOPS0 40B  OUR_EXEC=frozen  #6=untouched", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
