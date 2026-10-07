#!/usr/bin/env python3
"""TRACK A — validate paper_orbit opportunity_t against subsequent chain. No send."""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "paper004"))
import live001 as live  # noqa: E402
import paper004 as p4  # noqa: E402

SYN = Path("/home/louis/captures/paper_orbit/sync_state005.bin")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT = Path("/home/louis/captures/paper_orbit/PROVE_MONEY.json")


def parse_decisions(path: Path) -> list[dict]:
    b = path.read_bytes()
    off = 16
    rows = []
    while off + 16 <= len(b):
        ln, kind = struct.unpack_from("<IB", b, off)
        ts = struct.unpack_from("<Q", b, off + 8)[0]
        blen = ln - 12
        if blen < 0 or off + 16 + blen > len(b):
            break
        body = b[off + 16 : off + 16 + blen]
        if kind == 3 and len(body) >= 140 and body[139]:
            pidx = struct.unpack_from("<I", body, 64)[0]
            proto, direction = body[68], body[69]
            ain, minout, sver, pa, pb, rid, oa, oo, gp = struct.unpack_from(
                "<QQQQQIQQQ", body, 70
            )
            rows.append(
                {
                    "ts_ns": ts,
                    "sig": p4.b58encode(body[:64]),
                    "sig_hex": body[:64].hex(),
                    "pool_idx": pidx,
                    "proto": proto,
                    "n_dir": direction,
                    "n_ain": ain,
                    "min_out": minout,
                    "state_version": sver,
                    "pred_a": pa,
                    "pred_b": pb,
                    "route_id": rid,
                    "our_ain": oa,
                    "our_aout": oo,
                    "gross_profit": gp,
                    "our_dir": body[138],
                }
            )
        off += 16 + blen
    return rows


def main() -> int:
    live.load_dotenv()
    univ = json.loads(UNIV.read_text(encoding="utf-8"))
    pools = {int(p["idx"]): p for p in univ.get("pools") or []}
    decs = parse_decisions(SYN)
    print(f"TRACK A  valid_opp={len(decs)}", flush=True)
    rows = []
    for i, r in enumerate(decs):
        meta = pools.get(int(r["pool_idx"])) or {}
        r["pool"] = meta.get("pubkey")
        r["pool_kind"] = meta.get("proto")
        try:
            tx = p4.fetch_tx(r["sig"])
        except Exception as e:
            r["tx_err"] = str(e)
            rows.append(r)
            print(f"  {i} {r['sig'][:8]} tx_err {e}", flush=True)
            continue
        if not tx:
            r["n_landed"] = False
            r["subsequent"] = None
            rows.append(r)
            print(f"  {i} {r['sig'][:8]} unseen gp={r['gross_profit']}", flush=True)
            continue
        r["n_landed"] = True
        r["slot"] = tx.get("slot")
        r["err"] = (tx.get("meta") or {}).get("err")
        r["subsequent"] = (
            p4.later_arbs(r["pool"], r["sig"], int(r["slot"] or 0))
            if r.get("pool")
            else None
        )
        later = r["subsequent"] or {}
        hit = later.get("arb") or {}
        r["n_err"] = r.get("err")
        r["edge_exists"] = bool(r["n_landed"] and r["gross_profit"] > 0)
        r["taken"] = bool(hit)
        print(
            f"  {i} {r['sig'][:8]} pool={str(r.get('pool') or '')[:8]} "
            f"gp={r['gross_profit']} land={r['n_landed']} "
            f"later={bool(hit)} same_slot={hit.get('same_slot')} "
            f"taker={str(hit.get('searcher') or '')[:8]}",
            flush=True,
        )
        rows.append(r)
    report = {
        "n": len(rows),
        "landed": sum(1 for r in rows if r.get("n_landed")),
        "edge_exists": sum(1 for r in rows if r.get("edge_exists")),
        "later_arb": sum(1 for r in rows if r.get("taken")),
        "same_slot_arb": sum(
            1
            for r in rows
            if ((r.get("subsequent") or {}).get("arb") or {}).get("same_slot")
        ),
        "rows": rows,
        "note": (
            "gross_profit is cycle_size protocol units. "
            "later_arb = subsequent DLMM+Pump tx on the same pool. No send."
        ),
    }
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT}  n={report['n']} later_arb={report['later_arb']}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
