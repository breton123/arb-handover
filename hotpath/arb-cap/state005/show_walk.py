import json
from pathlib import Path

p = Path("/home/louis/captures/state005/SUMMARY.json")
s = json.loads(p.read_text())
print({k: s[k] for k in s if k != "rows"})
for r in s.get("rows") or []:
    if r.get("status") == "clean" and (r.get("walk") or 0) >= 1:
        print("MULTI", json.dumps({k: r[k] for k in r if k != "cmp_out"}, indent=2))
        print(r.get("cmp_out"))
        jp = Path("/home/louis/captures/state005") / f"{r['file']}.json"
        if jp.exists():
            t = json.loads(jp.read_text())
            ev = t["event"]
            print("sig", t["sig"])
            print("pair", t["pair"])
            print("slots", t.get("before", {}).get("slot"), t.get("tx_slot"), t.get("after", {}).get("slot"))
            print("event", {k: ev[k] for k in ev if k not in ("lb_pair", "from")})
