#!/usr/bin/env python3
"""Re-fetch the first opp_synced N and subsequent chain. No send."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "paper004"))
import live001 as live  # noqa: E402
import paper004 as p4  # noqa: E402

OUT = Path("/home/louis/captures/paper_orbit/OPP_SYNCED_1.json")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")


def main() -> int:
    live.load_dotenv()
    rec = json.loads(OUT.read_text(encoding="utf-8"))
    univ = json.loads(UNIV.read_text(encoding="utf-8"))
    pidx = int((rec.get("n") or {}).get("pool_idx") or -1)
    meta = next((p for p in univ.get("pools") or [] if int(p.get("idx") or -1) == pidx), {})
    rec["pool_b58"] = meta.get("pubkey") or rec.get("pool_b58")
    rec["pool_proto"] = meta.get("proto")
    rec["token"] = meta.get("token")
    rec["sol_side"] = meta.get("sol_side")
    sig = rec.get("sig") or ""
    if not sig and rec.get("sig_hex"):
        sig = p4.b58encode(bytes.fromhex(rec["sig_hex"]))
        rec["sig"] = sig
    print(f"retry {sig[:12]} pool={str(rec.get('pool_b58') or '')[:8]}", flush=True)
    tx = None
    try:
        tx = p4.fetch_tx(sig)
    except Exception as e:
        rec["tx_err"] = str(e)
    rec["n_landed"] = bool(tx)
    rec["err"] = (tx.get("meta") or {}).get("err") if tx else None
    rec["slot"] = tx.get("slot") if tx else None
    pool = rec.get("pool_b58") or ""
    if pool and sig:
        rec["subsequent"] = p4.later_arbs(pool, sig, int(rec.get("slot") or 0))
    else:
        rec["subsequent"] = None
    hit = ((rec.get("subsequent") or {}).get("arb")) or {}
    rec["taken"] = bool(hit)
    rec["same_slot"] = bool(hit.get("same_slot"))
    rec["edge_exists"] = bool(rec["n_landed"] and rec.get("err") is None)
    OUT.write_text(json.dumps(rec, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "landed": rec["n_landed"],
                "err": rec.get("err"),
                "slot": rec.get("slot"),
                "taken": rec["taken"],
                "same_slot": rec["same_slot"],
                "taker": (hit.get("searcher") or "")[:12],
                "later_sig": (hit.get("sig") or "")[:12],
                "pool": rec.get("pool_b58"),
                "token": rec.get("token"),
            },
            indent=2,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
