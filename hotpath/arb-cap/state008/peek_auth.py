#!/usr/bin/env python3
"""How stale is paper auth_slot vs shred slot on recent opp_synced."""
import json
from collections import defaultdict
from pathlib import Path

p = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
rows = []
with p.open("r", encoding="utf-8") as f:
    f.seek(0, 2)
    size = f.tell()
    f.seek(max(0, size - 4_000_000), 0)
    f.readline()
    for line in f:
        try:
            rec = json.loads(line)
        except Exception:
            continue
        if rec.get("kind") != "opp_synced":
            continue
        n = rec.get("n") or {}
        fr = rec.get("fresh") or {}
        auth = rec.get("auth_slot") or fr.get("auth_slot")
        shred = (rec.get("shred") or {}).get("slot") or fr.get("shred_slot")
        if auth is None or shred is None:
            continue
        rows.append({
            "idx": int(n.get("pool_idx") or -1),
            "auth": int(auth),
            "shred": int(shred),
            "age": rec.get("auth_age_slots", fr.get("age_slots")),
            "agen": rec.get("auth_generation"),
            "hash": rec.get("state_before_hash"),
        })

new = [r for r in rows if r["agen"] is not None]
print("recent", len(rows), "state009", len(new))
if not new:
    print("no STATE-009 journal yet")
    raise SystemExit(0)
rows = new
print("last", rows[-1])
ages = []
for r in rows:
    a = r["age"]
    if a is None:
        a = r["shred"] - r["auth"]
    ages.append(int(a))
s = sorted(ages)
print("age_slots p0", s[0], "p50", s[len(s)//2], "p99", s[int(0.99*(len(s)-1))], "p100", s[-1])
print("has_auth_generation", sum(1 for r in rows if r["agen"] is not None), "/", len(rows))
by = defaultdict(list)
for r in rows:
    by[r["idx"]].append(int(r["age"] if r["age"] is not None else r["shred"] - r["auth"]))
print("worst_idx")
for idx, ag in sorted(by.items(), key=lambda kv: -max(kv[1]))[:8]:
    print(" ", idx, "n", len(ag), "max_age", max(ag), "last_age", ag[-1])
