import json
from pathlib import Path

p = Path("/home/louis/captures/state005/SUMMARY.json")
s = json.loads(p.read_text()) if p.exists() else {}
print("n", s.get("n"), "exact", s.get("exact"), "stale", s.get("stale_before"))
walks = []
for r in s.get("rows") or []:
    ev = r.get("event") or {}
    start, end = ev.get("start_bin_id"), ev.get("end_bin_id")
    walk = None
    if start is not None and end is not None:
        walk = abs(int(end) - int(start))
    print(
        r.get("status"),
        r.get("exact"),
        r.get("file"),
        (r.get("sig") or "")[:8],
        "walk",
        walk,
        "rc",
        r.get("cmp_rc"),
    )
    out = r.get("cmp_out") or ""
    if "EXACT" in out or "MISMATCH" in out or "APPLY" in out:
        print(" ", out.strip().splitlines()[-1] if out.strip() else "")
