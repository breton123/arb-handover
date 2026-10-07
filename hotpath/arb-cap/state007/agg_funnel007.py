#!/usr/bin/env python3
"""STATE-007 PAPER gate: age32 vs mut_authoritative on the same FRAMED set."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
METRICS = Path("/home/louis/captures/state007/METRICS.json")
COMPARE = Path("/home/louis/captures/state007/COMPARE.json")
PLANE = Path("/home/louis/captures/state007/PLANE.json")
OUT = Path("/home/louis/captures/state007/FUNNEL.json")
HURDLE = 525_000
WINDOW_S = 30 * 60
LAMPORTS = 1_000_000_000


def _sol(n: int) -> str:
    return f"{n / LAMPORTS:.6f}"


def _add(s: dict, rec: dict) -> None:
    s["n"] += 1
    g = int(rec.get("cap_gross") or rec.get("gross") or 0)
    s["g"] += g
    if g > s["m"]:
        s["m"] = g


def _row(name: str, s: dict) -> str:
    n = s["n"]
    avg = (s["g"] // n) if n else 0
    return f"  {name:<34} {n:>6} {s['g']:>14} {avg:>12} {_sol(s['g']):>10}"


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else AUDIT)
    window = int(sys.argv[2]) if len(sys.argv) > 2 else WINDOW_S
    now = int(time.time())
    stages = {k: {"n": 0, "g": 0, "m": 0} for k in (
        "framed", "known", "searchable", "gross_pos", "cap_hurdle",
        "age32", "mut_auth", "race", "would_new", "updating",
    )}
    n_gate = 0
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"kind":"gate"' not in line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") != "gate":
                continue
            ts = int(rec.get("ts") or 0)
            if ts and now - ts > window:
                continue
            n_gate += 1
            _add(stages["framed"], rec)
            if not rec.get("known"):
                continue
            _add(stages["known"], rec)
            if not rec.get("searchable"):
                continue
            _add(stages["searchable"], rec)
            if rec.get("gross_pos"):
                _add(stages["gross_pos"], rec)
            if rec.get("updating"):
                _add(stages["updating"], rec)
            if not rec.get("cap_hurdle"):
                continue
            _add(stages["cap_hurdle"], rec)
            if rec.get("age32_all"):
                _add(stages["age32"], rec)
            if rec.get("mut_authoritative"):
                _add(stages["mut_auth"], rec)
            if rec.get("would_send_new"):
                _add(stages["would_new"], rec)
            if rec.get("exec_fam"):
                _add(stages["race"], rec)
    print(f"STATE-007 FUNNEL  last {window // 60} min  gates={n_gate}  hurdle={HURDLE}", flush=True)
    print(f"  {'stage':<34} {'N':>6} {'gross_sum':>14} {'avg':>12} {'SOL':>10}", flush=True)
    print(_row("CORE010 FRAMED", stages["framed"]), flush=True)
    print(_row("known", stages["known"]), flush=True)
    print(_row("searchable", stages["searchable"]), flush=True)
    print(_row("positive gross", stages["gross_pos"]), flush=True)
    print(_row("actual-size gross > hurdle", stages["cap_hurdle"]), flush=True)
    print(_row("old age32 pass", stages["age32"]), flush=True)
    print(_row("new mut_authoritative pass", stages["mut_auth"]), flush=True)
    print(_row("exec_fam (route0 compiler)", stages["race"]), flush=True)
    print(_row("would_send_new", stages["would_new"]), flush=True)
    print(_row("opp_suppressed_UPDATING", stages["updating"]), flush=True)
    met = {}
    if METRICS.exists():
        met = json.loads(METRICS.read_text(encoding="utf-8"))
        print(
            f"  stream updates={met.get('updates')} unique={met.get('unique_accounts')} "
            f"mutated={met.get('pools_mutated')} pub={met.get('published')} "
            f"updating={met.get('updating')} reconnects={met.get('reconnects')} "
            f"gaps={met.get('gaps')} decode_fail={met.get('decode_fail')}",
            flush=True,
        )
    if COMPARE.exists():
        cmp = json.loads(COMPARE.read_text(encoding="utf-8"))
        rows = cmp.get("rows") or []
        ok = sum(1 for r in rows if r.get("ok"))
        print(f"  compare ok={ok}/{len(rows)} mismatch={met.get('mismatch')} exact={met.get('exact')}", flush=True)
    if PLANE.exists():
        pl = json.loads(PLANE.read_text(encoding="utf-8"))
        print(
            f"  plane ready={pl.get('ready')} accounts={pl.get('n_account')} "
            f"pools={pl.get('n_pool')} host={pl.get('endpoint_host')}",
            flush=True,
        )
    OUT.write_text(
        json.dumps({"n_gate": n_gate, "window_s": window, "stages": stages, "metrics": met}, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
