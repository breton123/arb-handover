import json
from pathlib import Path

s = json.loads(Path("/home/louis/captures/state005/SUMMARY.json").read_text())
print("tally", {k: s[k] for k in s if k != "rows"})
for r in s.get("rows") or []:
    if r.get("status") not in ("clean", "interleave", "stale_before", "stale_after", "gone", "fail"):
        print("====", r.get("file"), r.get("status"), "walk", r.get("walk"))
        print((r.get("cmp_out") or "")[:1200])
