#!/usr/bin/env python3
"""Offline layout / coherence tests. No network. No tokens."""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from layout import parse_global_config, parse_pump_pool, token_amount  # noqa: E402
from pda import b58decode, b58encode, find_pda  # noqa: E402
from snapshot import fetch_coherent, parse_pool_key  # noqa: E402
from provider import CoherenceError  # noqa: E402


class FakeProv:
    def __init__(self, slots, maps):
        self.i = 0
        self.slots = slots
        self.maps = maps

    def name(self) -> str:
        return "fake"

    def get_slot(self) -> int:
        return 1

    def get_accounts(self, keys):
        i = min(self.i, len(self.slots) - 1)
        slot = self.slots[i]
        m = self.maps[i]
        self.i += 1
        return slot, {k: m[k] for k in keys if k in m}


def _pool_bytes(vb: bytes, vq: bytes, virt: int) -> bytes:
    buf = bytearray(269)
    buf[139:171] = vb
    buf[171:203] = vq
    buf[245:261] = int(virt).to_bytes(16, "little", signed=True)
    return bytes(buf)


def _tok(amount: int) -> bytes:
    buf = bytearray(72)
    buf[64:72] = int(amount).to_bytes(8, "little")
    return bytes(buf)


def _glob(lp: int, proto: int, creator: int, dis: int) -> bytes:
    buf = bytearray(57 + 32 * 8 + 8)
    buf[40:48] = int(lp).to_bytes(8, "little")
    buf[48:56] = int(proto).to_bytes(8, "little")
    buf[56] = dis
    buf[57 + 32 * 8 : 57 + 32 * 8 + 8] = int(creator).to_bytes(8, "little")
    return bytes(buf)


def main() -> int:
    fail = 0
    vb = bytes([0xB1]) * 32
    vq = bytes([0xB2]) * 32
    p = parse_pump_pool(_pool_bytes(vb, vq, 7))
    if p is None or p["virtual_quote"] != 7:
        print("FAIL parse pool")
        fail += 1
    else:
        print("ok    parse pool")
    if token_amount(_tok(99)) != 99:
        print("FAIL token")
        fail += 1
    else:
        print("ok    token amount")
    lp, pr, cr, d = parse_global_config(_glob(20, 5, 0, 0))
    if (lp, pr, cr, d) != (20, 5, 0, 0):
        print("FAIL global")
        fail += 1
    else:
        print("ok    global")
    raw = find_pda([b"global_config"], b58decode("pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"))
    print("ok    pda " + b58encode(raw)[:8] + "...")

    pool_b58 = b58encode(bytes([0xAB]) * 32)
    pool2_b58 = b58encode(bytes([0xAC]) * 32)
    vb_b58 = b58encode(vb)
    vq_b58 = b58encode(vq)
    from pda import pump_global_pk

    gpk = pump_global_pk()
    m0 = {
        pool_b58: _pool_bytes(vb, vq, 7),
        pool2_b58: _pool_bytes(vb, vq, 3),
        vb_b58: _tok(1000),
        vq_b58: _tok(2000),
        gpk: _glob(20, 5, 0, 0),
    }
    # discovery @ 11 discarded; vector context.slot 12 is the anchor
    prov = FakeProv([11, 12], [m0, m0])
    rows = fetch_coherent(prov, [pool_b58])
    if (len(rows) != 1 or rows[0]["slot"] != 12 or rows[0]["virtual_quote"] != 7
            or rows[0].get("commitment") != "finalized"):
        print("FAIL vector context is anchor")
        fail += 1
    else:
        print("ok    discovery slot ignored; vector context is anchor")

    # independent pools, independent anchors
    maps = [m0, m0, m0, m0]
    ind = FakeProv([1, 100, 1, 103], maps)
    rows = fetch_coherent(ind, [pool_b58, pool2_b58])
    if [r["slot"] for r in rows] != [100, 103]:
        print("FAIL independent anchors", [r["slot"] for r in rows])
        fail += 1
    else:
        print("ok    per-pool anchors")

    incomplete = FakeProv([1, 2], [m0, {pool_b58: m0[pool_b58]}])
    try:
        fetch_coherent(incomplete, [pool_b58])
        print("FAIL incomplete vector must not arm")
        fail += 1
    except CoherenceError:
        print("ok    incomplete vector not armed")

    hx = bytes(range(32)).hex()
    if parse_pool_key(hx) != b58encode(bytes(range(32))):
        print("FAIL hex pool key")
        fail += 1
    else:
        print("ok    hex disc line")

    if fail:
        print(f"\n{fail} failed")
        return 1
    print("\nanchor ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
