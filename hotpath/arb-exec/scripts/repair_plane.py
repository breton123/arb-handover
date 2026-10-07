#!/usr/bin/env python3
"""Refresh on-chain ALT contents and retry the two WAIT routes."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, "/home/louis/arb-cap")
sys.path.insert(0, "/home/louis/arb-exec/scripts")
import live001 as live
import exec_live002b as e
import alt_plane as ap

REPORT = Path("/home/louis/arb-cap/oneshot/ALT_PLANE.json")


def main() -> int:
    live.load_dotenv()
    payer = e.payer_kp()
    wallet = str(payer.pubkey())
    rep = json.loads(REPORT.read_text(encoding="utf-8"))
    waits = [r for r in rep.get("routes") or [] if not r.get("RACE_READY")]
    print(f"wait {len(waits)}", flush=True)
    alts = ap.register()
    routes = ap.resolve_all(wallet)
    by = {(r["dlmm"], r["pump"]): r for r in routes}
    for w in waits:
        key = (w["dlmm"], w["pump"])
        r = by.get(key)
        if not r:
            print(f"  gone {w['dlmm'][:8]}", flush=True)
            continue
        have = set()
        for a in alts:
            have.update(a.get("addresses") or [])
        miss = [x for x in r["need"] if x not in have]
        print(f"  {w['dlmm'][:8]} miss={len(miss)} { [x[:8] for x in miss[:8]] }", flush=True)
        if miss:
            best = max(alts, key=lambda a: 256 - len(a.get("addresses") or []))
            room = 256 - len(best.get("addresses") or [])
            extra = [x for x in miss if x not in (best.get("addresses") or [])][:room]
            if extra:
                print(f"  extend {best['pubkey'][:8]} +{len(extra)}", flush=True)
                e.alt_extend(payer, best["pubkey"], extra)
                alts = ap.register()
            still = [x for x in r["need"] if x not in set().union(*(a.get("addresses") or [] for a in alts))]
            if still:
                print(f"  leftover ALT n={len(r['need'])}", flush=True)
                pk = e.alt_create(payer, r["need"])
                alts = ap.register()
        comp = ap.compile_route(r)
        ata_ok = e.exists(r["user_quote"]) and e.exists(r["user_base"])
        race = bool(comp["dir0"] and comp["dir1"] and ata_ok)
        print(f"  retry {'READY' if race else 'WAIT '} d0={comp['dir0']} d1={comp['dir1']} {comp.get('err')}", flush=True)
        w.update({
            "alt": comp.get("alt"),
            "compile_dir0": comp["dir0"],
            "compile_dir1": comp["dir1"],
            "ata_ok": ata_ok,
            "RACE_READY": race,
            "err": comp.get("err"),
        })
    n_ready = sum(1 for r in rep["routes"] if r.get("RACE_READY"))
    rep["RACE_READY"] = n_ready
    rep["RACE_READY_DIRS"] = n_ready * 2
    rep["pct"] = n_ready / len(rep["routes"]) * 100.0 if rep["routes"] else 0
    e.publish_plane(alts, rep["routes"])
    REPORT.write_text(json.dumps(rep, indent=2) + "\n", encoding="utf-8")
    print(f"repair ready={n_ready}/{len(rep['routes'])} ({rep['pct']:.1f}%)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
