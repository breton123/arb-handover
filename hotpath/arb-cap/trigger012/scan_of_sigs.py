#!/usr/bin/env python3
"""Scan OrbitFlare FEEDCAP1 files for predecessor signatures.

Run on Frankfurt: python3 scan_of_sigs.py
Signatures are 64-byte ed25519 values. Presence in any shred payload
counts as feed-seen. Does not prove FRAMED.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if HERE.name != "trigger012":
    HERE = Path("/home/louis/arb-cap/trigger012")
PRED = HERE / "predecessors.jsonl"
CAPS = Path("/home/louis/captures/orbitflare")
OUT = Path("/home/louis/captures/trigger012/of_seen.json")
ALPH = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58d(s: str) -> bytes:
    n = 0
    for ch in s:
        n = n * 58 + ALPH.index(ch)
    raw = n.to_bytes(64, "big")
    pad = 0
    for ch in s:
        if ch == "1":
            pad += 1
        else:
            break
    raw = b"\x00" * pad + raw.lstrip(b"\x00")
    return raw.rjust(64, b"\x00")[-64:]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    rows = [json.loads(l) for l in PRED.read_text(encoding="utf-8").splitlines() if l.strip()]
    want = {}
    for r in rows:
        sig = r.get("pred_sig")
        if not sig:
            continue
        want[b58d(sig)] = r["pred_sig"]
    print(f"want {len(want)} pred sigs", flush=True)
    seen = set()
    caps = sorted(CAPS.glob("orbitflare-20260925-*.cap"))
    for cap in caps:
        print(f"scan {cap.name}", flush=True)
        with cap.open("rb") as f:
            magic = f.read(8)
            if magic != b"FEEDCAP1":
                continue
            f.seek(40)
            while True:
                hdr = f.read(24)
                if len(hdr) < 24:
                    break
                _rx_ns, _rx_tsc, ln, _seq = struct.unpack_from("<QQII", hdr)
                if ln <= 0 or ln > 2048:
                    break
                data = f.read(ln)
                if len(data) < ln:
                    break
                for raw, sig in list(want.items()):
                    if raw in data:
                        seen.add(sig)
        print(f"  seen {len(seen)}/{len(want)}", flush=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({"want": len(want), "seen": len(seen), "sigs": sorted(seen)}), encoding="utf-8")
    print(f"OF seen {len(seen)}/{len(want)} wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
