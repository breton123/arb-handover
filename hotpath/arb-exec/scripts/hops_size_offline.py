#!/usr/bin/env python3
"""Validate all 1530 hops v0 sizes against a 256-key ALT. No RPC. No send."""
from __future__ import annotations

import json
from pathlib import Path

from solders.compute_budget import set_compute_unit_limit, set_compute_unit_price
from solders.hash import Hash
from solders.instruction import AccountMeta, Instruction
from solders.message import MessageV0
from solders.null_signer import NullSigner
from solders.pubkey import Pubkey
from solders.transaction import VersionedTransaction

try:
    from solders.address_lookup_table_account import AddressLookupTableAccount
except ImportError:
    AddressLookupTableAccount = None

PLANE = Path("/home/louis/arb-cap/fam6/hops_plane.json")
OUT = Path("/home/louis/arb-cap/fam6/SIZE.json")
V0_MAX = 1232
PROGRAM = "CnddPhKV1fnKE7ic5nSJcoVq2XFmQ9daE3tuqc2u6qTt"
PAYER = "HHNzjTABPAbPY5uEXU7NTvfmcD9RYTdXtLntyKAtpXLX"


def pk(i: int) -> str:
    raw = (i + 1).to_bytes(32, "big")
    return str(Pubkey.from_bytes(raw))


def main() -> int:
    plane = json.loads(PLANE.read_text(encoding="utf-8"))
    alt_addrs = [pk(1000 + i) for i in range(256)]
    alt = None
    if AddressLookupTableAccount is not None:
        alt = AddressLookupTableAccount(
            Pubkey.from_string(pk(999)),
            [Pubkey.from_string(a) for a in alt_addrs],
        )
    bh = Hash.from_string("11111111111111111111111111111111")
    payer = Pubkey.from_string(PAYER)
    prog = Pubkey.from_string(PROGRAM)
    rows = []
    over = 0
    for r in plane.get("routes") or []:
        hops = r.get("hops") or []
        n_acc = 9 + sum(h[3] for h in hops)
        keys = [PAYER] + [pk(10 + i) for i in range(n_acc - 1)]
        # ALT must contain the instruction keys or try_compile leaves them static.
        cover = keys[1:]
        pad = [pk(3000 + i) for i in range(max(0, 256 - len(cover)))]
        this_alt = None
        if AddressLookupTableAccount is not None:
            this_alt = AddressLookupTableAccount(
                Pubkey.from_string(pk(999)),
                [Pubkey.from_string(a) for a in (cover + pad)[:256]],
            )
        metas = []
        for i, k in enumerate(keys):
            metas.append(AccountMeta(Pubkey.from_string(k), i == 0, i == 0 or i >= 1))
        data = bytes.fromhex(r["ix"])
        ixs = [
            set_compute_unit_limit(300_000),
            set_compute_unit_price(1),
            Instruction(prog, data, metas),
        ]
        lut = [this_alt] if this_alt is not None else []
        msg = MessageV0.try_compile(payer, ixs, lut, bh)
        n = len(bytes(VersionedTransaction(msg, [NullSigner(payer)])))
        ok = n <= V0_MAX
        if not ok:
            over += 1
        rows.append({"id": r["id"], "seq": r["seq"], "ok": ok, "raw": n, "n_acc": n_acc})
    obj = {
        "n": len(rows),
        "ok": sum(1 for s in rows if s["ok"]),
        "over_1232": over,
        "v0_max": V0_MAX,
        "mode": "offline_geometry",
        "rows": rows,
    }
    OUT.write_text(json.dumps(obj) + "\n", encoding="utf-8")
    mx = max((s["raw"] for s in rows), default=0)
    print(f"SIZE  n={obj['n']} ok={obj['ok']} over={over} max_raw={mx}", flush=True)
    return 0 if obj["ok"] == 1530 and over == 0 else 5


if __name__ == "__main__":
    raise SystemExit(main())
