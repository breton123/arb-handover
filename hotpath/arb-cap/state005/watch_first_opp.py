#!/usr/bin/env python3
"""Wait for the first opp_synced audit, then follow the chain. No send."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "paper004"))
import live001 as live  # noqa: E402
import paper004 as p4  # noqa: E402

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
OUT = Path("/home/louis/captures/paper_orbit/OPP_SYNCED_1.json")


def follow_first(path: Path) -> dict:
    off = 0
    print("WATCH  first opp_synced  (SYNCED S → N → S' → cycle_size)", flush=True)
    while True:
        if path.exists() and path.stat().st_size > off:
            data = path.read_bytes()
            chunk = data[off:]
            off = len(data)
            for line in chunk.splitlines():
                if not line.strip():
                    continue
                rec = json.loads(line)
                if rec.get("kind") == "opp_synced":
                    return rec
        time.sleep(0.25)


def main() -> int:
    live.load_dotenv()
    rec = follow_first(AUDIT)
    hx = rec.get("sig_hex") or ""
    sig = p4.b58encode(bytes.fromhex(hx)) if hx else ""
    rec["sig"] = sig
    print(
        f"FIRST  {sig[:12]} ver={rec.get('state_version_before')} "
        f"gp={((rec.get('arb') or {}).get('gross'))} "
        f"ain={((rec.get('arb') or {}).get('amount_in'))}",
        flush=True,
    )
    rec["n_landed"] = False
    rec["subsequent"] = None
    if sig:
        try:
            tx = p4.fetch_tx(sig)
        except Exception as e:
            rec["tx_err"] = str(e)
            tx = None
        if tx:
            rec["n_landed"] = True
            rec["slot"] = tx.get("slot")
            rec["err"] = (tx.get("meta") or {}).get("err")
            pool = rec.get("pool") or ""
            if len(pool) == 64:
                pool = p4.b58encode(bytes.fromhex(pool))
                rec["pool_b58"] = pool
            rec["subsequent"] = p4.later_arbs(pool, sig, int(rec.get("slot") or 0))
    hit = ((rec.get("subsequent") or {}).get("arb")) or {}
    rec["edge_exists"] = bool(rec.get("n_landed") and not rec.get("err")
                              and ((rec.get("arb") or {}).get("gross") or 0) > 0)
    rec["taken"] = bool(hit)
    rec["same_slot"] = bool(hit.get("same_slot"))
    rec["note"] = (
        "First opp_synced: pool was SYNCED immediately before N. "
        "gross is cycle_size protocol units. No send."
    )
    OUT.write_text(json.dumps(rec, indent=2) + "\n", encoding="utf-8")
    print(
        f"chain  land={rec['n_landed']} err={rec.get('err')} "
        f"later={rec['taken']} same_slot={rec['same_slot']} "
        f"taker={str(hit.get('searcher') or '')[:8]}  wrote {OUT}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
