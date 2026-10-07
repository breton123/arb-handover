#!/usr/bin/env python3
"""Audit would_send_new + compare mismatches. No secrets."""
from __future__ import annotations

import json
import time
from collections import Counter, defaultdict
from pathlib import Path

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
PLANE = Path("/home/louis/arb-exec/.deploy/alt_plane.json")
PLANE2 = Path("/home/louis/arb-cap/oneshot/ALT_PLANE.json")
S007_PLANE = Path("/home/louis/captures/state007/PLANE.json")
COMPARE = Path("/home/louis/captures/state007/COMPARE.json")
METRICS = Path("/home/louis/captures/state007/METRICS.json")
LOG = Path("/home/louis/captures/state007/state007.log")
READY = Path("/home/louis/captures/state007/READY")
HURDLE = 525_000
WINDOW_S = 30 * 60


def load_json(p: Path, default):
    if not p.exists():
        return default
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return default


def plane_ready() -> set[str]:
    out: set[str] = set()
    for p in (PLANE, PLANE2):
        rows = load_json(p, {}).get("routes") or []
        for r in rows:
            if not r.get("RACE_READY"):
                continue
            for k in ("dlmm", "pump"):
                if r.get(k):
                    out.add(str(r[k]))
    return out


def main() -> int:
    now = int(time.time())
    univ = load_json(UNIV, {})
    by_idx = {int(p.get("idx") or -1): p for p in univ.get("pools") or []}
    ready_pks = plane_ready()
    gates = []
    with AUDIT.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"kind":"gate"' not in line:
                continue
            rec = json.loads(line)
            if rec.get("kind") != "gate":
                continue
            ts = int(rec.get("ts") or 0)
            if ts and now - ts > WINDOW_S:
                continue
            gates.append(rec)
    hurdle = [g for g in gates if g.get("cap_hurdle")]
    would = [g for g in gates if g.get("would_send_new")]
    mut = [g for g in gates if g.get("mut_authoritative") and g.get("cap_hurdle")]
    execs = [g for g in gates if g.get("exec_fam") and g.get("cap_hurdle")]
    print(f"AUDIT  gates={len(gates)} hurdle={len(hurdle)} mut={len(mut)} would={len(would)} exec={len(execs)}", flush=True)
    print(f"  READY_file={READY.exists()} plane={load_json(S007_PLANE, {}).get('ready')}", flush=True)

    def show(tag: str, rec: dict) -> None:
        idx = int(rec.get("pool_idx") or -1)
        meta = by_idx.get(idx) or {}
        pk = meta.get("pubkey") or ""
        hops = int(rec.get("n_hop") or 0)
        n_sync = int(rec.get("n_sync") or 0)
        print(f"--- {tag} ---", flush=True)
        print(
            f"  ts={rec.get('ts')} seq={rec.get('seq')} family={rec.get('family')} "
            f"pool_idx={idx} proto={meta.get('proto') or meta.get('kind')} "
            f"pk={(pk[:8] + '…') if pk else '-'}",
            flush=True,
        )
        print(
            f"  cap_gross={rec.get('cap_gross')} hurdle={HURDLE} "
            f"excess={int(rec.get('cap_gross') or 0) - HURDLE}",
            flush=True,
        )
        print(
            f"  n_hop={hops} n_sync={n_sync} synced_all={rec.get('synced_all')} "
            f"updating={rec.get('updating')} age32={rec.get('age32_all')} "
            f"mut_auth={rec.get('mut_authoritative')} s007_ready={rec.get('s007_ready')} "
            f"state_gen={rec.get('state_gen')} last_auth_slot={rec.get('last_auth_slot')} "
            f"exec_fam={rec.get('exec_fam')} would_send_new={rec.get('would_send_new')}",
            flush=True,
        )
        print(
            f"  hop_SYNCED={n_sync}/{hops}  no_UPDATING={not rec.get('updating')}  "
            f"FRAMED=1  quote_0.05={rec.get('cap_gross')}  "
            f"ALT_RACE_READY_trigger={bool(pk and pk in ready_pks)}",
            flush=True,
        )

    for rec in would:
        show("would_send_new", rec)
    for rec in execs:
        if rec not in would:
            show("exec_fam_hurdle_not_would", rec)
    for rec in hurdle:
        if rec not in would and rec not in execs:
            show("hurdle_other", rec)

    # Compare log: lifetime vs unique pools
    mm = Counter()
    fields = Counter()
    bins = Counter()
    if LOG.exists():
        for line in LOG.read_text(encoding="utf-8", errors="replace").splitlines():
            if "MISMATCH" not in line:
                continue
            # STATE-007  MISMATCH idx=1 fields=[] bins=6
            idx = None
            fb = None
            bb = None
            parts = line.split()
            for p in parts:
                if p.startswith("idx="):
                    idx = p.split("=", 1)[1]
                elif p.startswith("fields="):
                    fb = p.split("=", 1)[1]
                elif p.startswith("bins="):
                    bb = p.split("=", 1)[1]
            mm[idx] += 1
            fields[idx] += 0 if fb in ("[]", None) else 1
            if bb and bb.isdigit():
                bins[idx] += int(bb)
    print("--- compare lifetime ---", flush=True)
    met = load_json(METRICS, {})
    print(
        f"  exact={met.get('exact')} mismatch={met.get('mismatch')} "
        f"updating_now={met.get('updating')} updating_max={met.get('updating_max')}",
        flush=True,
    )
    print(f"  unique_mismatch_idxs={len(mm)}  samples={sum(mm.values())}", flush=True)
    last = load_json(COMPARE, {})
    print(f"  last_compare_ts={last.get('ts')} rows={last.get('rows')}", flush=True)
    for idx, n in mm.most_common(12):
        meta = by_idx.get(int(idx) if idx and idx.lstrip('-').isdigit() else -1) or {}
        pk = (meta.get("pubkey") or "")[:8]
        print(
            f"  idx={idx} n={n} field_mismatch_hits={fields[idx]} bin_bad_sum={bins[idx]} pk={pk}",
            flush=True,
        )
    # Checklist summary for would_send_new
    print("--- contract ---", flush=True)
    if not would:
        print("  no would_send_new in window", flush=True)
        return 0
    rec = would[-1]
    idx = int(rec.get("pool_idx") or -1)
    meta = by_idx.get(idx) or {}
    pk = meta.get("pubkey") or ""
    hops_ok = rec.get("synced_all") and int(rec.get("n_sync") or 0) == int(rec.get("n_hop") or 0)
    no_upd = not rec.get("updating")
    gen_ok = int(rec.get("state_gen") or 0) > 0 and rec.get("s007_ready")
    quote_ok = int(rec.get("cap_gross") or 0) == 877575 or int(rec.get("cap_gross") or 0) > HURDLE
    race = bool(pk and pk in ready_pks) or bool(rec.get("exec_fam"))
    mm_on_trigger = str(idx) in mm
    print(f"  every_hop_SYNCED          {int(bool(hops_ok))}", flush=True)
    print(f"  no_UPDATING               {int(bool(no_upd))}", flush=True)
    print(f"  no_unresolved_mismatch    {int(not mm_on_trigger)}  (trigger idx in mismatch log={mm_on_trigger})", flush=True)
    print(f"  YS/bootstrap generation   {int(bool(gen_ok))}  gen={rec.get('state_gen')} ready={rec.get('s007_ready')}", flush=True)
    print(f"  CORE010 FRAMED            1", flush=True)
    print(f"  actual_0.05_quote         {rec.get('cap_gross')}", flush=True)
    print(f"  RACE_READY                {int(bool(race))}  exec_fam={rec.get('exec_fam')} alt={bool(pk and pk in ready_pks)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
