#!/usr/bin/env python3
"""Compare reconstructed txs to a truth jsonl (Yellowstone or entry_truth).

Truth rows need sig_hex (128 hex) and slot. Optional entry, tx_i, version.
Recon rows: same keys from wire_to_state003 --dump-txs.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def load(path: Path) -> dict[str, dict]:
    out: dict[str, dict] = {}
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            sig = (r.get("sig_hex") or "").lower()
            if len(sig) < 64:
                continue
            out[sig] = r
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recon", required=True)
    ap.add_argument("--truth", required=True)
    ap.add_argument("--limit-slots", type=int, default=0)
    args = ap.parse_args()
    recon = load(Path(args.recon))
    truth = load(Path(args.truth))
    if args.limit_slots:
        slots = sorted({int(r["slot"]) for r in recon.values() if "slot" in r})
        keep = set(slots[: args.limit_slots])
        recon = {k: v for k, v in recon.items() if int(v.get("slot", -1)) in keep}
    hit = miss_slot = miss_sig = miss_idx = miss_ver = 0
    compared = 0
    for sig, t in truth.items():
        if sig not in recon:
            miss_sig += 1
            continue
        compared += 1
        r = recon[sig]
        slot_ok = True
        if "slot" in t and "slot" in r and int(t["slot"]) != int(r["slot"]):
            miss_slot += 1
            slot_ok = False
        ti = t.get("index", t.get("tx_i"))
        ri = r.get("index", r.get("tx_i"))
        if ti is not None and ri is not None and int(ti) != int(ri):
            miss_idx += 1
        if "version" in t and "version" in r and int(t["version"]) != int(r["version"]):
            miss_ver += 1
        if slot_ok:
            hit += 1
    print(
        f"truth={len(truth)} recon={len(recon)} compared={compared} "
        f"slot_match={hit} slot_mismatch={miss_slot} index_mismatch={miss_idx} "
        f"version_mismatch={miss_ver} truth_only={miss_sig}"
    )
    ok = compared > 0 and miss_slot == 0 and miss_idx == 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
