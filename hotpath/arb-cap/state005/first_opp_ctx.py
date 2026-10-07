#!/usr/bin/env python3
"""Context for first opp_synced: pool mints, partner, recent DLMM+Pump. No send."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "paper004"))
import live001 as live  # noqa: E402
import paper004 as p4  # noqa: E402

UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT = Path("/home/louis/captures/paper_orbit/OPP_SYNCED_1.json")


def main() -> int:
    live.load_dotenv()
    rec = json.loads(OUT.read_text(encoding="utf-8"))
    univ = json.loads(UNIV.read_text(encoding="utf-8"))
    pidx = int((rec.get("n") or {}).get("pool_idx") or -1)
    meta = next((p for p in univ.get("pools") or [] if int(p.get("idx") or -1) == pidx), {})
    tok = meta.get("token") or rec.get("token")
    partners = [
        p
        for p in univ.get("pools") or []
        if p.get("proto") == "pump" and p.get("token") == tok
    ]
    pool = rec.get("pool_b58") or meta.get("pubkey")
    recent = []
    try:
        sigs = p4.rpc("getSignaturesForAddress", [pool, {"limit": 20}]) or []
    except Exception as e:
        sigs = []
        rec["sig_scan_err"] = str(e)
    for ent in sigs[:12]:
        try:
            tx = p4.fetch_tx(ent["signature"])
        except Exception:
            continue
        if not tx or (tx.get("meta") or {}).get("err"):
            continue
        pids = p4.ix_pids(tx)
        recent.append(
            {
                "sig": ent["signature"],
                "slot": ent.get("slot"),
                "dlmm": live.DLMM in pids if hasattr(live, "DLMM") else "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo" in pids,
                "pump": "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA" in pids,
                "searcher": (p4.tx_keys(tx) or [""])[0][:12],
            }
        )
    rec["partners"] = [{"idx": p.get("idx"), "pubkey": p.get("pubkey")} for p in partners]
    rec["recent_pool_txs"] = recent
    rec["recent_dlmm_pump"] = [r for r in recent if r.get("dlmm") and r.get("pump")]
    OUT.write_text(json.dumps(rec, indent=2) + "\n")
    print(
        json.dumps(
            {
                "pool": pool,
                "token": tok,
                "sol_side": meta.get("sol_side"),
                "partners": rec["partners"],
                "recent_dlmm_pump": rec["recent_dlmm_pump"],
                "auth_slot": rec.get("auth_slot"),
                "ver": rec.get("state_version_before"),
                "gp": (rec.get("arb") or {}).get("gross"),
                "ain_sol": ((rec.get("arb") or {}).get("amount_in") or 0) / 1e9,
                "final": (rec.get("arb") or {}).get("final"),
            },
            indent=2,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
