#!/usr/bin/env python3
"""Reclassify existing STATE-005 triples. Exclusions are not model failures."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture import classify_compare, tally, walk_len, write_summary  # noqa: E402


def main() -> int:
    outdir = Path(sys.argv[1] if len(sys.argv) > 1 else "/home/louis/captures/state005")
    cmp_bin = sys.argv[2] if len(sys.argv) > 2 else "/home/louis/arb-core/build/state005"
    rows = []
    for path in sorted(outdir.glob("triple_*.json")):
        rec = json.loads(path.read_text(encoding="utf-8"))
        ev = rec.get("event") or {}
        walk = walk_len(ev)
        stem = path.stem
        sig = rec.get("sig") or ""
        if rec.get("stale_before") or (
            ev and rec.get("before", {}).get("active_id") != ev.get("start_bin_id")
        ):
            rows.append(
                {
                    "sig": sig,
                    "status": "stale_before",
                    "file": stem,
                    "walk": walk,
                    "event": ev,
                }
            )
            print(f"{stem} stale_before walk={walk}", flush=True)
            continue
        tri = path.with_suffix(".tri")
        if not tri.exists() or not ev:
            rows.append({"sig": sig, "status": "no_event", "file": stem, "walk": walk})
            continue
        p = subprocess.run(
            [cmp_bin, str(tri)], check=False, capture_output=True, text=True, timeout=10
        )
        status = classify_compare(p.stdout, p.returncode, rec.get("after") or {}, ev)
        rows.append(
            {
                "sig": sig,
                "status": status,
                "file": stem,
                "walk": walk,
                "exact": status == "clean",
                "cmp_rc": p.returncode,
                "cmp_out": p.stdout,
                "event": {
                    "start_bin_id": ev.get("start_bin_id"),
                    "end_bin_id": ev.get("end_bin_id"),
                    "fee": ev.get("fee"),
                    "amount_out": ev.get("amount_out"),
                },
            }
        )
        print(f"{stem} {status} walk={walk} rc={p.returncode}", flush=True)
    write_summary(outdir, rows)
    print(json.dumps(tally(rows), indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
