#!/usr/bin/env python3
"""Aggregate opp_searchable by protocol sequence.

Priority is fresh ∩ cap-profitable ∩ frequent, not searchable alone.
"""
from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
OUT = Path("/home/louis/captures/paper_orbit/SEARCHABLE_SEQ.json")
HURDLE = 525_000


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else AUDIT)
    buckets: dict[str, dict] = defaultdict(lambda: {
        "n": 0,
        "cap_ok": 0,
        "fresh_n": 0,
        "cap_ok_and_fresh_n": 0,
        "raceable_n": 0,
        "gross_fresh": 0,
        "gross_fresh_n": 0,
        "gross_fresh_max": 0,
        "cap_gross_fresh": 0,
        "cap_gross_fresh_max": 0,
        "gross": 0,
        "cap_gross": 0,
        "gross_max": 0,
        "cap_gross_max": 0,
        "sendable": 0,
        "age_sum": 0,
        "age_n": 0,
    })
    n = 0
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"opp_searchable"' not in line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") != "opp_searchable":
                continue
            seq = rec.get("seq") or rec.get("bucket") or "?"
            hop = int(rec.get("n_hop") or 0)
            key = f"{seq}|h{hop}|{rec.get('bucket')}"
            b = buckets[key]
            b["n"] += 1
            n += 1
            g = int(rec.get("gross") or 0)
            cg = int(rec.get("cap_gross") or 0)
            cap_ok = bool(rec.get("cap_ok"))
            fr = rec.get("fresh") or {}
            fresh = bool(fr.get("ok"))
            sendable = bool(fr.get("sendable"))
            age = fr.get("age_slots")
            b["gross"] += g
            b["cap_gross"] += cg
            if g > b["gross_max"]:
                b["gross_max"] = g
            if cg > b["cap_gross_max"]:
                b["cap_gross_max"] = cg
            if cap_ok:
                b["cap_ok"] += 1
            if sendable:
                b["sendable"] += 1
            if isinstance(age, int):
                b["age_sum"] += age
                b["age_n"] += 1
            if fresh:
                b["fresh_n"] += 1
                b["gross_fresh"] += g
                b["gross_fresh_n"] += 1
                b["cap_gross_fresh"] += cg
                if g > b["gross_fresh_max"]:
                    b["gross_fresh_max"] = g
                if cg > b["cap_gross_fresh_max"]:
                    b["cap_gross_fresh_max"] = cg
                if cap_ok:
                    b["cap_ok_and_fresh_n"] += 1
                    if cg > HURDLE:
                        b["raceable_n"] += 1
    rows = []
    for key, b in buckets.items():
        seq, hop, bucket = key.split("|")
        rows.append({
            "seq": seq,
            "n_hop": int(hop[1:]),
            "bucket": bucket,
            "searchable_n": b["n"],
            "pct": (100.0 * b["n"] / n) if n else 0.0,
            "fresh_n": b["fresh_n"],
            "cap_ok": b["cap_ok"],
            "cap_ok_and_fresh_n": b["cap_ok_and_fresh_n"],
            "raceable_n": b["raceable_n"],
            "sendable": b["sendable"],
            "age_avg": (b["age_sum"] // b["age_n"]) if b["age_n"] else None,
            "gross_fresh_avg": (b["gross_fresh"] // b["fresh_n"]) if b["fresh_n"] else 0,
            "gross_fresh_max": b["gross_fresh_max"],
            "cap_gross_fresh_avg": (b["cap_gross_fresh"] // b["fresh_n"]) if b["fresh_n"] else 0,
            "cap_gross_fresh_max": b["cap_gross_fresh_max"],
            "cap_gross_avg": b["cap_gross"] // b["n"] if b["n"] else 0,
            "cap_gross_max": b["cap_gross_max"],
        })
    rows.sort(key=lambda r: (-r["raceable_n"], -r["cap_ok_and_fresh_n"], -r["searchable_n"]))
    OUT.write_text(json.dumps({"n": n, "hurdle": HURDLE, "rows": rows}, indent=2) + "\n", encoding="utf-8")
    print(
        f"SEARCHABLE  n={n}  hurdle={HURDLE}  "
        f"priority=fresh ∩ cap_ok ∩ >hurdle",
        flush=True,
    )
    print(
        f"  {'seq':<22} {'n':>4} {'fresh':>6} {'cap∧f':>6} {'race':>5} "
        f"{'age':>5} {'g_f_avg':>10} {'g_f_max':>10}",
        flush=True,
    )
    for r in rows[:16]:
        print(
            f"  {r['seq']:<22} {r['searchable_n']:>4} {r['fresh_n']:>6} "
            f"{r['cap_ok_and_fresh_n']:>6} {r['raceable_n']:>5} "
            f"{str(r['age_avg'] if r['age_avg'] is not None else '-'):>5} "
            f"{r['cap_gross_fresh_avg']:>10} {r['cap_gross_fresh_max']:>10}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
