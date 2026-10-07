#!/usr/bin/env python3
"""Brutal two-venue closure audit. Compiler is source of truth."""
from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

DUMP = Path("/home/louis/arb-cap/fam6/ROUTES.json")
PLANE = Path("/home/louis/arb-cap/fam6/hops_plane.json")
S007 = Path("/home/louis/captures/state007/PLANE.json")
OUT = Path("/home/louis/arb-cap/fam6/AUDIT.json")


def main() -> int:
    d = json.loads(DUMP.read_text(encoding="utf-8"))
    hops = json.loads(PLANE.read_text(encoding="utf-8")) if PLANE.exists() else {}
    s007 = json.loads(S007.read_text(encoding="utf-8")) if S007.exists() else {}
    n = int(d.get("n_route") or 0)
    exec_n = int(d.get("executable") or 0)
    miss = int(d.get("exec_missing") or 0)
    fam255 = int(d.get("family255") or 0)
    tmpl = int(hops.get("n_template") or 0)
    ready = Path("/home/louis/captures/state007/READY").exists()
    n_pool_s007 = int((s007 or {}).get("n_pool") or 0)
    n_pool_univ = int(d.get("n_pool") or 0)
    if ready and n_pool_s007 >= n_pool_univ and n_pool_univ > 0:
        state_ok = exec_n
        state_note = f"STATE-007 READY n_pool={n_pool_s007} covers univ n={n_pool_univ}"
    else:
        state_ok = 0
        state_note = f"STATE-007 ready={ready} n_pool={n_pool_s007} univ={n_pool_univ}"
    audit = {
        "compiled_closed_routes": n,
        "searchable": exec_n,
        "state_representable": state_ok,
        "route_executable": exec_n,
        "template_compiled": tmpl,
        "alt_ata_ready": int(hops.get("race_ready") or 0),
        "RACE_READY": int(hops.get("race_ready") or 0),
        "EXEC_MISSING": miss,
        "family255": fam255,
        "fam0": d.get("fam0"),
        "fam5": d.get("fam5"),
        "fam6": d.get("fam6"),
        "state_note": state_note,
        "holes": {
            "hot_paper_unfrozen_source": True,
            "hot_paper_live_binary": False,
            "arbhops0_deployed": False,
            "alt_published": False,
        },
        "complete": miss == 0 and fam255 == 0 and n == exec_n == tmpl
        and int(hops.get("race_ready") or 0) == n,
    }
    OUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print("AUDIT  {DLMM, Pump} closure", flush=True)
    for k in (
        "compiled_closed_routes", "searchable", "state_representable",
        "route_executable", "template_compiled", "alt_ata_ready",
        "RACE_READY", "EXEC_MISSING", "family255",
    ):
        print(f"  {k:<26} {audit[k]}", flush=True)
    print(f"  complete={audit['complete']}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
