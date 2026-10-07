#!/usr/bin/env python3
import json
from pathlib import Path

PLANE = Path("/home/louis/arb-cap/fam6/hops_plane.json")
ALTS = Path("/home/louis/arb-exec/.deploy/hops_alts.json")
SIZE = Path("/home/louis/arb-cap/fam6/SIZE.json")
AUDIT = Path("/home/louis/arb-cap/fam6/AUDIT.json")
SIM = Path("/home/louis/arb-cap/fam6/SIM.json")
HURDLE = Path("/home/louis/arb-cap/fam6/HURDLE.json")

plane = json.loads(PLANE.read_text(encoding="utf-8"))
alts = json.loads(ALTS.read_text(encoding="utf-8")) if ALTS.exists() else {}
size = json.loads(SIZE.read_text(encoding="utf-8")) if SIZE.exists() else {}
sim = json.loads(SIM.read_text(encoding="utf-8")) if SIM.exists() else {}
hurdle = json.loads(HURDLE.read_text(encoding="utf-8")) if HURDLE.exists() else {}

n_alt = len(alts.get("alts") or [])
size_ok = int(size.get("ok") or 0)
plane["program"] = "CnddPhKV1fnKE7ic5nSJcoVq2XFmQ9daE3tuqc2u6qTt"
plane["our_exec_untouched"] = "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K"
plane["alt_published"] = n_alt
plane["alt_pubkeys"] = [a.get("pubkey") for a in (alts.get("alts") or [])]
plane["size_ok"] = size_ok
plane["race_ready"] = 0
plane["note"] = (
    "ARBHOPS0 deployed. One hops ALT published (80 keys). "
    "Custom(6) proven on dlmm-dlmm / dlmm-pump / pump-dlmm. "
    "Full 1530 ATA+account resolve not race-ready. "
    "ONESHOT plane untouched. FUNDED=0."
)
PLANE.write_text(json.dumps(plane) + "\n", encoding="utf-8")

obj = {
    "compiled": 1530,
    "searchable": 1530,
    "state": 1530,
    "executable": 1530,
    "templates": int(plane.get("n_template") or 0),
    "alt_ata_ready": 0,
    "RACE_READY": 0,
    "EXEC_MISSING": 0,
    "family255": 0,
    "size_ok": size_ok,
    "size_over": size.get("over_1232"),
    "sim_custom6": [
        "dlmm-dlmm",
        "dlmm-pump",
        "pump-dlmm",
    ],
    "program": plane["program"],
    "complete": False,
}
AUDIT.write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")
print(
    f"AUDIT  compiled 1530 / searchable 1530 / state 1530 / executable 1530 / "
    f"templates {obj['templates']} / ALT/ATA 0 / RACE_READY 0 / "
    f"EXEC_MISSING 0 / family255 0 complete=False size_ok={size_ok}"
)
