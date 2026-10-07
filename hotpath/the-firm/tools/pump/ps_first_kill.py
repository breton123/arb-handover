#!/usr/bin/env python3
"""First kill per pool from NEW ps-log."""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path


def main() -> None:
    path = Path(sys.argv[1])
    first_kill: dict[str, dict] = {}
    kill_rc: Counter[str] = Counter()
    n_apply_kill = 0
    n_lut_kill = 0
    first_apply_rows = []
    for line in path.open(encoding="utf-8"):
        r = json.loads(line)
        if not r.get("killed_exact"):
            continue
        rc = r.get("rc")
        if rc == "APPLY":
            n_apply_kill += 1
            if len(first_apply_rows) < 8:
                first_apply_rows.append(r)
        elif rc == "LUT":
            n_lut_kill += 1
        for p in r.get("pools") or []:
            if p.get("grade_before") != "EXACT" or p.get("grade_after") != "DIRTY":
                continue
            pk = p.get("pk_hex") or ""
            if pk in first_kill:
                continue
            first_kill[pk] = {
                "rc": rc,
                "slot": r.get("slot"),
                "sig": r.get("sig_hex"),
                "dep_class": r.get("dep_class"),
                "apply_code": r.get("apply_code"),
                "n_pump": r.get("n_pump"),
                "n_alt": r.get("n_alt"),
                "lut_resolved": r.get("lut_resolved"),
            }
            kill_rc[rc] += 1
    print("first EXACT->DIRTY by pumpstate rc:", dict(kill_rc))
    print("killed_exact row counts: APPLY", n_apply_kill, "LUT", n_lut_kill)
    print()
    print("=== first APPLY rejects that killed EXACT (up to 8 txs) ===")
    for r in first_apply_rows:
        print(
            f"slot={r.get('slot')} dep_class={r.get('dep_class')} "
            f"apply_code={r.get('apply_code')} n_pump={r.get('n_pump')} "
            f"n_alt={r.get('n_alt')} lut_resolved={r.get('lut_resolved')}"
        )
        print(f"  sig={r.get('sig_hex')}")
        for p in r.get("pools") or []:
            if p.get("grade_before") == "EXACT":
                print(
                    f"  pool={p.get('pk_hex')} auth_ready={p.get('auth_ready')} "
                    f"anchor={p.get('anchor_slot')}"
                )
    hot = "016d5671c72d408924d6cb344da4cffcfa9ae4f2d8c19e02a583351f201410b8"
    print()
    print("hot pool first kill:", first_kill.get(hot))


if __name__ == "__main__":
    main()
