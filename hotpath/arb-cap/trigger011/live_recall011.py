#!/usr/bin/env python3
"""Live MEV.live recall instrumentation for TRIGGER-011.

The reference hour (2464 / $2085 DLMM+Pump) is the market size.
This script reports how our live detector would classify fetched
predecessor-like traffic and what the current PAPER funnel is emitting.

Does not invent S'. FUNDED remains 0.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(r"c:\Users\louis\Desktop\TheMoneyMaker")
MEV = ROOT / "arb-cap" / "regress" / "mev_hour.json"
EVENTS = Path("/home/louis/captures/trigger011/events.jsonl")
ALT_MET = Path("/home/louis/captures/trigger011/alt_plane.json")
PAPER = Path("/home/louis/captures/paper_orbit/paper_trig011.log")
OUT = ROOT / "arb-cap" / "trigger011" / "live_recall011.json"


def tail_funnel(path: Path) -> dict:
    if not path.exists():
        return {}
    last = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "trig gen=" in line:
            last["funnel"] = line.strip()
        if line.startswith("TRIGGER-011"):
            last["banner"] = line.strip()
        if line.startswith("PAPER-ORBIT  up="):
            last["up"] = line.strip()
    return last


def event_counts(path: Path) -> dict:
    n = {"relevant_unknown": 0, "alt_miss": 0, "exact": 0, "other": 0, "lines": 0}
    if not path.exists():
        return n
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        n["lines"] += 1
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        k = rec.get("klass")
        if k == 4:
            n["exact"] += 1
        elif k == 5:
            n["relevant_unknown"] += 1
        elif k == 3:
            n["alt_miss"] += 1
        else:
            n["other"] += 1
    return n


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    mev = json.loads(MEV.read_text(encoding="utf-8"))
    dp = mev["agg"]["dlmm_pump"]
    all_ = mev["agg"]["all"]
    ev = event_counts(EVENTS)
    paper = tail_funnel(PAPER)
    altm = json.loads(ALT_MET.read_text(encoding="utf-8")) if ALT_MET.exists() else {}
    doc = {
        "ref_hour": {
            "dlmm_pump_n": dp["n"],
            "dlmm_pump_usd": dp["usd"],
            "all_n": all_["n"],
            "all_usd": all_["usd"],
            "top_n": len(mev.get("top_dlmm_pump") or []),
        },
        "live_events": ev,
        "alt_plane": altm,
        "paper": paper,
        "note": (
            "Predecessor-level match against every successful MEV.live arb "
            "requires the slim-block predecessor pass used for late-121. "
            "This report is the live funnel + event journal. "
            "Unknown CPI never produces S'."
        ),
    }
    OUT.write_text(json.dumps(doc, indent=2), encoding="utf-8")
    print("LIVE MEV.live reference hour")
    print(f"  DLMM/Pump successful  {dp['n']}  ${dp['usd']:.0f}")
    print(f"  all successful        {all_['n']}  ${all_['usd']:.0f}")
    print("live trigger011 events", ev)
    print("alt_plane", altm)
    print("paper", paper.get("funnel") or paper.get("banner") or "no paper log")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
