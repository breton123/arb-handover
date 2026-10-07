#!/usr/bin/env python3
"""TRIGGER RECALL against recorded shreds.

The 431-slot Mriya N+1 labels live in a different slot range than the
Frankfurt TVU dump. This script measures that fact, then scans for any
overlapping account bytes anyway.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ALPH = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58decode(s: str) -> bytes:
    n = 0
    for c in s:
        n = n * 58 + ALPH.index(c)
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    h = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    raw = b"\x00" * pad + h
    return raw[-32:] if len(raw) >= 32 else raw.rjust(32, b"\x00")


def load_shreds(path: Path) -> tuple[bytes, list[tuple[int, int, int]]]:
    """Return (file bytes, list of (file_off, plen, slot))."""
    b = path.read_bytes()
    off = 8
    recs = []
    while off + 2 <= len(b):
        plen = struct.unpack_from("<H", b, off)[0]
        off += 2
        pkt = b[off : off + plen]
        slot = struct.unpack_from("<Q", pkt, 65)[0] if len(pkt) >= 73 else 0
        recs.append((off, plen, slot))
        off += plen
    return b, recs


def main() -> None:
    if len(sys.argv) != 3:
        print("usage: trigger_recall.py triggers.json shreds.arbrx", file=sys.stderr)
        raise SystemExit(1)
    labels = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    blob, recs = load_shreds(Path(sys.argv[2]))
    slots = [s for _, _, s in recs]
    feed_min, feed_max = min(slots), max(slots)
    triggers = labels["triggers"]
    n = len(triggers)
    slot_hit = 0
    acc_hit = 0
    for t in triggers:
        if feed_min <= t["slot"] <= feed_max:
            slot_hit += 1
        found = False
        for acc in t.get("overlap_accs") or []:
            raw = b58decode(acc)
            if raw in blob:
                found = True
                break
        if found:
            acc_hit += 1

    print("TRIGGER RECALL")
    print()
    print(f"labeled Mriya N+1 examples   {n}  (of {labels.get('n_pairs_full', '?')} parsed pairs)")
    print(f"trigger slots                {labels['trigger_slots']['min']} .. {labels['trigger_slots']['max']}")
    print(f"feed slots                   {feed_min} .. {feed_max}")
    print(f"slot overlap                 {slot_hit}/{n}")
    print(f"overlap-account bytes in feed {acc_hit}/{n}")
    print()
    print("Mriya N+1 triggers:")
    print(f"  seen in feed               {100.0 * slot_hit / n:.1f}%")
    print(f"  relevance classifier hit   n/a  (no matching shreds)")
    print(f"  pool identified            n/a")
    print(f"  instruction recoverable    n/a")
    print()
    print("first usable shred relative to trigger:")
    print("  shred offset p50            n/a")
    print("  time estimate               n/a")
    print()
    if slot_hit == 0:
        print("No raw shreds for the 431-slot labels. RPC getBlock is not a shred.")
        print("CORE-002 offset recovery is measured on the TVU dump, not on these triggers.")


if __name__ == "__main__":
    main()
