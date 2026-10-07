#!/usr/bin/env python3
"""Best-effort funnel from journals written before kind=gate existed."""
from __future__ import annotations

import json
import sys
from pathlib import Path

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
HURDLE = 525_000
LAMPORTS = 1_000_000_000


def _sol(n: int) -> str:
    return f"{n / LAMPORTS:.6f}"


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else AUDIT)
    framed = known = unknown = search = gpos = cap = cap_h = fresh = send = 0
    synced_n = 0
    gsum = csum = fsum = 0
    gmax = cmax = 0
    drop = drop_age = drop_mut = 0
    synced_kind = 0
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            k = rec.get("kind")
            if k == "frame" and rec.get("class") == "framed":
                framed += 1
            elif k == "framed_pool":
                known += 1
            elif k == "unknown_pool":
                unknown += 1
            elif k == "opp_searchable":
                search += 1
                g = int(rec.get("gross") or 0)
                cg = int(rec.get("cap_gross") or 0)
                if g > 0:
                    gpos += 1
                    gsum += g
                    if g > gmax:
                        gmax = g
                if rec.get("cap_ok"):
                    cap += 1
                    csum += cg
                    if cg > cmax:
                        cmax = cg
                    if cg > HURDLE:
                        cap_h += 1
                fr = rec.get("fresh") or {}
                if fr.get("sendable"):
                    synced_n += 1
                if fr.get("ok"):
                    fresh += 1
                    fsum += cg
            elif k == "fresh_drop":
                drop += 1
                if not rec.get("age_ok"):
                    drop_age += 1
                if rec.get("mut_ok"):
                    drop_mut += 1
            elif k == "opp_synced":
                synced_kind += 1
    print("LIFETIME (unwindowed, pre-gate journal)  NOT last-30-min", flush=True)
    print(f"  CORE010 FRAMED                 {framed:>6}", flush=True)
    print(f"  known pool                     {known:>6}", flush=True)
    print(f"  unknown pool                   {unknown:>6}", flush=True)
    print(
        f"  searchable cycle               {search:>6}  "
        f"gross_sum={gsum} ({_sol(gsum)} SOL)  max={gmax}",
        flush=True,
    )
    print(f"  positive gross                 {gpos:>6}", flush=True)
    print(
        f"  actual-size > 0                {cap:>6}  "
        f"cap_sum={csum} ({_sol(csum)} SOL)  max={cmax}",
        flush=True,
    )
    print(
        f"  actual-size > {HURDLE:<12} {cap_h:>6}",
        flush=True,
    )
    print(f"  trigger SYNCED (sendable)      {synced_n:>6}", flush=True)
    print(f"  AUTH <= 32                     {fresh:>6}  cap_sum={fsum}", flush=True)
    print(f"  opp_synced / RACE_READY path   {synced_kind:>6}", flush=True)
    print(
        f"  fresh_drop (send-path only)    {drop:>6}  "
        f"age_fail={drop_age}  mut_ok={drop_mut}",
        flush=True,
    )
    if cap > 0 and fresh == 0:
        print(
            "  verdict: B  +EV actual-size exists; 32-slot AUTH killed all of them",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
