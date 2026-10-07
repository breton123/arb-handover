#!/usr/bin/env python3
"""Fetch a coherent Pump compact snapshot. Cold start only.

Buffer OF first. For each pool, vault addresses are discovered, then
pool + vaults + global_config are fetched in **one** getMultipleAccounts.
That response's **finalized** `context.slot` is this row's `anchor_slot`.
GetSlot is not the boundary. Do not arm a processed/open bank.

Does not print tokens.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from envload import load_env  # noqa: E402
from layout import parse_global_config, parse_pump_pool, token_amount  # noqa: E402
from pda import b58decode, b58encode, pump_global_pk  # noqa: E402
from provider import CoherenceError  # noqa: E402
from shyft import ShyftProvider  # noqa: E402


def _hex(raw: bytes) -> str:
    return raw.hex()


def _provider(name: str):
    if name == "shyft":
        return ShyftProvider()
    raise SystemExit("unknown provider: " + name)


def fetch_one(prov, pool_pk: str, gpk: str) -> dict | None:
    """Discover vaults, then one coherent vector. Anchor = second context.slot."""
    _s_disc, disc = prov.get_accounts([pool_pk])
    raw = disc.get(pool_pk)
    if not raw:
        return None
    p = parse_pump_pool(raw)
    if p is None:
        return None
    vb = b58encode(p["vault_base"])
    vq = b58encode(p["vault_quote"])
    s_anchor, accs = prov.get_accounts([pool_pk, vb, vq, gpk])
    pool2 = accs.get(pool_pk)
    glob = accs.get(gpk)
    if pool2 is None or glob is None:
        return None
    p2 = parse_pump_pool(pool2)
    if p2 is None:
        return None
    rb = token_amount(accs.get(b58encode(p2["vault_base"]), b""))
    rq = token_amount(accs.get(b58encode(p2["vault_quote"]), b""))
    if rb is None or rq is None or rb == 0 or rq == 0:
        return None
    lp, proto, creator, disabled = parse_global_config(glob)
    return {
        "pool_hex": _hex(b58decode(pool_pk)),
        "vault_base_hex": _hex(p2["vault_base"]),
        "vault_quote_hex": _hex(p2["vault_quote"]),
        "slot": s_anchor,
        "commitment": "finalized",
        "provider": prov.name(),
        "reserve_base": rb,
        "reserve_quote": rq,
        "virtual_quote": p2["virtual_quote"],
        "lp_fee_bps": lp,
        "protocol_fee_bps": proto,
        "creator_fee_bps": creator,
        "disabled": disabled,
    }


def parse_pool_key(line: str) -> str | None:
    s = line.strip()
    if not s or s.startswith("#"):
        return None
    if len(s) == 64:
        try:
            raw = bytes.fromhex(s)
        except ValueError:
            return None
        if len(raw) != 32:
            return None
        return b58encode(raw)
    return s


def fetch_coherent(prov, pools: list[str]) -> list[dict]:
    gpk = pump_global_pk()
    rows = []
    for pk in pools:
        row = fetch_one(prov, pk, gpk)
        if row is not None:
            rows.append(row)
    if not rows:
        raise CoherenceError("no pools decoded")
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--provider", default="shyft")
    ap.add_argument("--pools", required=True, help="file of base58 pool pubkeys")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    load_env()
    keys = []
    for ln in Path(args.pools).read_text(encoding="utf-8").splitlines():
        pk = parse_pool_key(ln)
        if pk:
            keys.append(pk)
    if not keys:
        print("no pool keys", file=sys.stderr)
        return 2
    prov = _provider(args.provider)
    hint = None
    try:
        hint = prov.get_slot()
    except Exception:
        hint = None
    try:
        rows = fetch_coherent(prov, keys)
    except CoherenceError as ex:
        print("anchor failed: " + type(ex).__name__, file=sys.stderr)
        return 1
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    with Path(args.out).open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")
    slots = sorted({int(r["slot"]) for r in rows})
    msg = f"anchor  provider={prov.name()}  pools={len(rows)}  anchors={slots}  out={args.out}"
    if hint is not None:
        msg += f"  getslot_hint={hint}"
    print(msg, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
