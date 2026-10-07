import json
from pathlib import Path
p = Path("/home/louis/captures/paper_orbit/liveuniv.json")
m = json.loads(p.read_text())
print("n", m.get("n"), "keys", list(m)[:8])
print("protos", [x.get("proto") or x.get("kind") for x in (m.get("pools") or [])[:12]])
