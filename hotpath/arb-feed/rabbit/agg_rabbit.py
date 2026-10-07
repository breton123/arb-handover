#!/usr/bin/env python3
"""Print RABBIT-001 race snapshot. Observe only."""
from __future__ import annotations

import json
from pathlib import Path

METRICS = Path("/home/louis/captures/rabbit/METRICS.json")


def main() -> int:
    if not METRICS.exists():
        print("RABBIT-001  no metrics yet")
        return 1
    s = json.loads(METRICS.read_text(encoding="utf-8"))
    r = s.get("race") or {}
    print(
        f"RABBIT-001  up={s.get('uptime_s')}s  recon={s.get('reconnects')} "
        f"err={s.get('errors')} slot={s.get('stream_slot')}",
        flush=True,
    )
    print(
        f"  rabbit={s.get('rabbit_unique')} of_framed={s.get('of_framed_unique')} "
        f"matched={s.get('matched')} rab_only={s.get('rabbit_only')} "
        f"of_only={s.get('of_only')}",
        flush=True,
    )
    print(
        f"  OF_seen_by_rabbit={s.get('of_seen_by_rabbit_pct'):.1f}% "
        f"rabbit_seen_by_OF={s.get('rabbit_seen_by_of_pct'):.1f}%",
        flush=True,
    )
    print(
        f"  valid={s.get('valid')} invalid={s.get('invalid')} votes={s.get('votes')} "
        f"proto={s.get('proto')} known={s.get('known_pool')} route0={s.get('route0')} "
        f"hot_n={s.get('hot_decode')}",
        flush=True,
    )
    print(
        f"  race rabbit_first={r.get('rabbit_first')} ({r.get('rabbit_first_pct'):.1f}%) "
        f"of_first={r.get('of_first')} ({r.get('of_first_pct'):.1f}%) ties={r.get('ties')}",
        flush=True,
    )
    for name in ("all", "dlmm", "pump", "known", "route0", "searchable"):
        row = r.get(name) or {}
        print(
            f"  lead[{name}] n={row.get('n')} p50={row.get('p50')} "
            f"p90={row.get('p90')} p99={row.get('p99')} "
            f"min={row.get('min')} max={row.get('max')} "
            f"r={row.get('rabbit_first')} of={row.get('of_first')}",
            flush=True,
        )
    print(f"  hist_rabbit {r.get('hist_rabbit_lead')}", flush=True)
    print(f"  hist_of     {r.get('hist_of_lead')}", flush=True)
    print(f"  local_lat   {s.get('local_lat_ns')}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
