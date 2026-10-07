#!/usr/bin/env python3
"""Join OLD APPLY_OK (--pred) with NEW --ps-log for the same freeze cap."""
from __future__ import annotations

import json
import sys
from collections import Counter, defaultdict
from pathlib import Path


def load_jsonl(path: Path) -> list[dict]:
    rows = []
    if not path.exists():
        return rows
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def pool_key(p: dict) -> str:
    return p.get("pk_hex") or str(p.get("id"))


def main() -> int:
    old_pred = Path(sys.argv[1] if len(sys.argv) > 1 else "run/ab1/old_pred.jsonl")
    new_ps = Path(sys.argv[2] if len(sys.argv) > 2 else "run/ab1/new_ps.jsonl")
    old_ok = load_jsonl(old_pred)
    new_rows = load_jsonl(new_ps)
    by_sig: dict[str, dict] = {}
    for r in new_rows:
        sig = r.get("sig_hex") or ""
        if sig and sig not in by_sig:
            by_sig[sig] = r

    print(f"OLD APPLY_OK preds: {len(old_ok)}")
    print(f"NEW ps-log rows:    {len(new_rows)}")
    print()

    rc_hist: Counter[str] = Counter()
    prev: Counter[str] = Counter()
    missing = 0
    for row in old_ok:
        sig = row.get("sig_hex") or ""
        n = by_sig.get(sig)
        if n is None:
            missing += 1
            rc_hist["MISSING_IN_NEW"] += 1
            continue
        rc_hist[n.get("rc") or "?"] += 1
        for p in n.get("pools") or []:
            pk = p.get("prev_kill") or ""
            gb = p.get("grade_before") or ""
            prev[f"{gb}/{n.get('rc')}/prev={pk or 'none'}"] += 1

    print("NEW rc for every OLD APPLY_OK signature")
    for k, v in rc_hist.most_common():
        print(f"  {k:<20} {v}")
    print()
    print("NEW pool snapshot on those signatures (grade_before / rc / prev_kill)")
    for k, v in prev.most_common(20):
        print(f"  {k:<40} {v}")
    print()

    print("=== first 10 OLD APPLY_OK ===")
    for i, row in enumerate(old_ok[:10]):
        sig = row.get("sig_hex") or ""
        n = by_sig.get(sig)
        print(f"\n[{i}] slot={row.get('slot')} pool_id={row.get('pool_id')}")
        print(f"    sig {sig[:32]}...")
        print(f"    old → APPLY_OK")
        if n is None:
            print("    new → not in ps-log")
            continue
        print(
            f"    new → rc={n.get('rc')} lut_resolved={n.get('lut_resolved')} "
            f"n_pump={n.get('n_pump')} n_alt={n.get('n_alt')} "
            f"dep_class={n.get('dep_class')} apply_code={n.get('apply_code')} "
            f"killed_exact={n.get('killed_exact')}"
        )
        for p in n.get("pools") or []:
            print(
                f"      pool {p.get('pk_hex', '')[:16]}… "
                f"before={p.get('grade_before')} after={p.get('grade_after')} "
                f"auth_ready={p.get('auth_ready')} anchor={p.get('anchor_slot')} "
                f"prev_kill={p.get('prev_kill') or 'none'}"
            )

    # Per-pool NEW chronology
    events: dict[str, list[tuple[int, dict, dict]]] = defaultdict(list)
    for idx, r in enumerate(new_rows):
        for p in r.get("pools") or []:
            events[pool_key(p)].append((idx, r, p))

    old_first_ok: dict[int, dict] = {}
    for row in old_ok:
        pid = row.get("pool_id")
        if pid not in old_first_ok:
            old_first_ok[pid] = row

    print("\n=== per-pool (NEW) first LUT kill vs later GAP ===")
    n_show = 0
    for pk, evs in sorted(events.items(), key=lambda kv: -len(kv[1])):
        first_lut = next(
            (e for e in evs if e[1].get("rc") == "LUT" and e[2].get("killed_exact")),
            None,
        )
        first_gap = next((e for e in evs if e[1].get("rc") == "GAP"), None)
        gaps_after_lut = 0
        if first_lut is not None:
            lut_i = first_lut[0]
            gaps_after_lut = sum(
                1 for e in evs if e[0] > lut_i and e[1].get("rc") == "GAP"
            )
        if first_lut is None and first_gap is None:
            continue
        n_show += 1
        if n_show > 24:
            break
        print(f"\npool {pk[:16]}…  events={len(evs)}")
        if first_lut:
            r, p = first_lut[1], first_lut[2]
            print(
                f"  first LUT_STATIC slot={r.get('slot')} "
                f"before={p.get('grade_before')} after={p.get('grade_after')} "
                f"sig={str(r.get('sig_hex'))[:24]}…"
            )
        if first_gap:
            r, p = first_gap[1], first_gap[2]
            print(
                f"  first GAP slot={r.get('slot')} "
                f"before={p.get('grade_before')} prev_kill={p.get('prev_kill') or 'none'}"
            )
        print(f"  GAP after that LUT_STATIC: {gaps_after_lut}")

    print(f"\nOLD APPLY_OK sigs missing in NEW ps-log: {missing}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
