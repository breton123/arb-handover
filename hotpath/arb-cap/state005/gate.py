import json
from pathlib import Path

p = Path("/home/louis/captures/state005/SUMMARY.json")
s = json.loads(p.read_text()) if p.exists() else {}
keys = (
    "clean",
    "clean_multi_bin",
    "interleave",
    "stale_before",
    "stale_after",
    "model_mismatch",
    "n",
)
print(" ".join(f"{k}={s.get(k)}" for k in keys))
print("walks", s.get("clean_walks"))
cleans = [r for r in (s.get("rows") or []) if r.get("status") == "clean"]
for r in cleans:
    ev = r.get("event") or {}
    print(
        "CLEAN",
        (r.get("file") or "")[:28],
        "walk",
        r.get("walk"),
        ev.get("start_bin_id"),
        "->",
        ev.get("end_bin_id"),
    )
gate = int(s.get("clean") or 0) >= 10 and int(s.get("clean_multi_bin") or 0) >= 1
print("GATE", "OPEN" if gate else "WAIT")
