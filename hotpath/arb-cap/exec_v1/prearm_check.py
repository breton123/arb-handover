#!/usr/bin/env python3
"""Print every pre-arm gate. Exit 0 only when a single funded shot is allowed to be asked for.

Does not set FUNDED, does not arm, does not send, does not move SOL.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-exec" / "scripts"))
import exec_gates as gates  # noqa: E402

READY = Path(gates.AUTH_READY_DEFAULT)
LIVE_READY = gates.current_auth_ready()
ALIAS = Path(gates.STATE007_READY_ALIAS)
ARMED = Path("/home/louis/arb-cap/oneshot/ARMED")
SOCK = Path("/home/louis/arb-cap/oneshot/swqos.sock")
AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
PLANE = Path("/home/louis/arb-exec/.deploy/alt_plane.json")
REPORT = Path(__file__).resolve().parent / "template_audit.json"


def main() -> int:
    fails = 0

    def gate(name: str, ok: bool, detail: str) -> None:
        nonlocal fails
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)
        if not ok:
            fails += 1

    funded = os.environ.get("FUNDED", "0") == "1"
    gate("FUNDED_STAYS_0", not funded, "unset FUNDED before asking for the shot")
    path, err = gates.resolve_auth_ready(os.environ.get("AUTH_READY"))
    gate("AUTH_PATH", err is None and path == gates.AUTH_READY_DEFAULT, err or path)
    gate("STATE008_ARM_READY", READY.exists(), f"oneshot_arm={READY}")
    cap, cap_err = gates.current_auth_cap()
    gate(
        "LIVE_AUTH_READY",
        LIVE_READY.exists() and cap_err is None,
        f"cap={cap} ready={LIVE_READY} err={cap_err}",
    )
    gate("STATE007_DOES_NOT_ARM", True, f"alias_exists={ALIAS.exists()} ignored")
    gate("NOT_ARMED", not ARMED.exists(), str(ARMED))
    gate("RACER_SOCKET", SOCK.exists(), str(SOCK))
    gate("AUDIT_FILE", AUDIT.exists(), str(AUDIT))
    plane_ok = False
    detail = "missing"
    if PLANE.exists():
        try:
            doc = json.loads(PLANE.read_text(encoding="utf-8"))
            rows = doc.get("routes") or []
            both = sum(1 for r in rows if r.get("RACE_READY") and r.get("tmpl0") and r.get("tmpl1"))
            plane_ok = both > 0
            detail = f"tmpl_both={both}/{len(rows)}"
        except json.JSONDecodeError:
            detail = "unreadable"
    gate("PLANE_TEMPLATES", plane_ok, detail)
    amb: set[str] = set()
    amb_detail = "plane missing"
    plane_read = False
    if PLANE.exists():
        try:
            pdoc = json.loads(PLANE.read_text(encoding="utf-8"))
            amb = gates.ambiguous_pool_keys(pdoc.get("routes") or [])
            plane_read = True
            amb_detail = f"shared_pubkeys={len(amb)}"
        except json.JSONDecodeError:
            amb_detail = "unreadable"
    race = 0
    clean: list = []
    sim_detail = "missing template_audit.json"
    if REPORT.exists():
        try:
            audited = json.loads(REPORT.read_text(encoding="utf-8"))
            routes = audited.get("routes") or []
            race = sum(1 for r in routes if r.get("RACE_READY"))
            hurdle = audited.get("wire_hurdle")
            clean = [
                r for r in routes
                if r.get("RACE_READY")
                and r.get("dlmm") not in amb
                and r.get("pump") not in amb
            ]
            sim_detail = (
                f"race_ready={race}/{len(routes)} unambiguous={len(clean)} "
                f"wire_hurdle={hurdle}"
            )
            if clean:
                sim_detail += " pools=" + ",".join(r["dlmm"][:8] for r in clean)
        except json.JSONDecodeError:
            sim_detail = "unreadable"
    gate("UNSIGNED_SIM_RACE_READY", plane_read and race > 0, sim_detail)
    gate(
        "UNAMBIGUOUS_SHOT_POOL",
        plane_read and len(clean) > 0,
        amb_detail if not plane_read else f"{amb_detail} unambiguous_shot_pools={len(clean)}",
    )
    gate("WIRE_HURDLE", gates.HURDLE == 525_000 and gates.HURDLE == gates.wire_hurdle(), str(gates.HURDLE))
    print(f"RESULT  fails={fails}", flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
