#!/usr/bin/env python3
"""30-minute gate funnel. Diagnoses A (no search) / B (freshness) / C (no executor)."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
PLANE = Path("/home/louis/arb-exec/.deploy/alt_plane.json")
PLANE2 = Path("/home/louis/captures/paper_orbit/ALT_PLANE.json")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT = Path("/home/louis/captures/paper_orbit/GATE_FUNNEL.json")
HURDLE = 525_000
WINDOW_S = 30 * 60
LAMPORTS = 1_000_000_000


def _plane_ready() -> set[str]:
    out: set[str] = set()
    for p in (PLANE, PLANE2):
        if not p.exists():
            continue
        try:
            rows = json.loads(p.read_text(encoding="utf-8")).get("routes") or []
        except Exception:
            continue
        for r in rows:
            if not r.get("RACE_READY"):
                continue
            for k in ("dlmm", "pump"):
                if r.get(k):
                    out.add(str(r[k]))
    return out


def _idx_pub() -> dict[int, str]:
    if not UNIV.exists():
        return {}
    try:
        pools = json.loads(UNIV.read_text(encoding="utf-8")).get("pools") or []
    except Exception:
        return {}
    out: dict[int, str] = {}
    for p in pools:
        idx = p.get("idx")
        pk = p.get("pubkey")
        if idx is not None and pk:
            out[int(idx)] = str(pk)
    return out


def _sol(lamports: int) -> str:
    return f"{lamports / LAMPORTS:.6f}"


def _add(stage: dict, rec: dict) -> None:
    stage["n"] += 1
    g = int(rec.get("cap_gross") or rec.get("gross") or 0)
    stage["gross"] += g
    if g > stage["max"]:
        stage["max"] = g


def _row(name: str, s: dict) -> dict:
    n = s["n"]
    return {
        "stage": name,
        "n": n,
        "gross_sum": s["gross"],
        "gross_avg": (s["gross"] // n) if n else 0,
        "gross_max": s["max"],
        "sol_sum": s["gross"] / LAMPORTS,
    }


def load_gates(path: Path, now: int, window: int) -> list[dict]:
    rows = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"kind":"gate"' not in line and '"kind": "gate"' not in line:
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
            rows.append(rec)
    return rows


def print_table(title: str, rows: list[dict], n_win: int) -> None:
    print(title, flush=True)
    print(f"  window_s={n_win}  hurdle={HURDLE}  unit=lamports / SOL", flush=True)
    print(
        f"  {'stage':<34} {'N':>6} {'gross_sum':>14} {'avg':>12} {'max':>12} {'SOL':>10}",
        flush=True,
    )
    for r in rows:
        print(
            f"  {r['stage']:<34} {r['n']:>6} {r['gross_sum']:>14} "
            f"{r['gross_avg']:>12} {r['gross_max']:>12} {_sol(r['gross_sum']):>10}",
            flush=True,
        )


def verdict(rows: dict[str, dict]) -> str:
    framed = rows["framed"]["n"]
    search = rows["searchable"]["n"]
    cap = rows["cap_hurdle"]["n"]
    age32 = rows["age32"]["n"]
    mut = rows["mut_fresh"]["n"]
    exec_n = rows["exec"]["n"]
    race = rows["race"]["n"]
    if framed == 0:
        return "NO_DATA"
    if search * 20 < framed and cap == 0:
        return "A  search finds very little — add venues/routes"
    if cap > 0 and age32 == 0 and mut >= max(1, cap // 4):
        return "B  search + economics exist; 32-slot AUTH is starving them — stop route work, fix state delivery"
    if cap > 0 and age32 == 0 and mut == 0:
        return "B  search + economics exist; nothing is AUTH-fresh — fix state plane"
    if (age32 > 0 or mut > 0) and exec_n == 0 and race == 0:
        return "C  fresh +EV exists; execution family missing — build executor"
    if race > 0:
        return "SEND_PATH  something is crossing every gate"
    return "MIXED  inspect stage drops"


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else AUDIT)
    window = int(sys.argv[2]) if len(sys.argv) > 2 else WINDOW_S
    now = int(time.time())
    gates = load_gates(path, now, window)
    plane = _plane_ready()
    idx_pub = _idx_pub()

    names = (
        "framed", "known", "routed", "searchable", "gross_pos",
        "cap_hurdle", "synced", "age32", "exec", "race", "mut_fresh",
    )
    stages = {k: {"n": 0, "gross": 0, "max": 0} for k in names}

    for rec in gates:
        _add(stages["framed"], rec)
        if not rec.get("known"):
            continue
        _add(stages["known"], rec)
        if not rec.get("routed"):
            continue
        _add(stages["routed"], rec)
        if not rec.get("searchable"):
            continue
        _add(stages["searchable"], rec)
        if rec.get("gross_pos"):
            _add(stages["gross_pos"], rec)
        if not rec.get("cap_hurdle"):
            continue
        _add(stages["cap_hurdle"], rec)
        if rec.get("synced_all"):
            _add(stages["synced"], rec)
        else:
            continue
        mut_ok = bool(rec.get("mut_ok_all"))
        if mut_ok:
            _add(stages["mut_fresh"], rec)
        if rec.get("age32_all"):
            _add(stages["age32"], rec)
        else:
            continue
        if rec.get("exec_fam"):
            _add(stages["exec"], rec)
        else:
            continue
        pk = idx_pub.get(int(rec.get("pool_idx") or -1))
        if pk and pk in plane:
            _add(stages["race"], rec)

    table = [
        _row("CORE010 FRAMED", stages["framed"]),
        _row("known pool", stages["known"]),
        _row("route affected", stages["routed"]),
        _row("searchable cycle", stages["searchable"]),
        _row("positive gross", stages["gross_pos"]),
        _row("actual-size gross > costs", stages["cap_hurdle"]),
        _row("all involved states SYNCED", stages["synced"]),
        _row("AUTH <= 32 slots", stages["age32"]),
        _row("execution family available", stages["exec"]),
        _row("RACE_READY", stages["race"]),
    ]
    counter = [_row("SYNCED ∩ mut-fresh ∩ >costs  (no 32-slot)", stages["mut_fresh"])]
    v = verdict(stages)
    src = "gate" if gates else "empty"
    out = {
        "window_s": window,
        "n_gate": len(gates),
        "source": src,
        "hurdle": HURDLE,
        "rows": table,
        "counterfactual": counter,
        "verdict": v,
    }
    OUT.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    if not gates:
        print("GATE FUNNEL  no kind=gate lines in window — paper_orbit not yet writing gates", flush=True)
        return 0
    print_table(f"GATE FUNNEL  last {window // 60} min  n={len(gates)}", table, window)
    print("", flush=True)
    print_table("COUNTERFACTUAL  STATE-003 mutation model", counter, window)
    print("", flush=True)
    print(f"  verdict: {v}", flush=True)
    cap = stages["cap_hurdle"]["n"]
    age = stages["age32"]["n"]
    mut = stages["mut_fresh"]["n"]
    print(
        f"  +EV actual-size={cap}  AUTH<=32={age}  mut-fresh={mut}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
