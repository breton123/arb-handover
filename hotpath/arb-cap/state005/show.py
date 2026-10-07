import json
from pathlib import Path

s = json.loads(Path("/home/louis/captures/state005/SUMMARY.json").read_text())
print("n", s.get("n"), "exact", s.get("exact"), "stale", s.get("stale_before"))
for r in s.get("rows") or []:
    print(
        r.get("status"),
        r.get("exact"),
        r.get("file"),
        (r.get("sig") or "")[:8],
        "rc",
        r.get("cmp_rc"),
    )
    out = r.get("cmp_out")
    if out:
        print(out)