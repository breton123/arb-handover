#!/usr/bin/env python3
"""Deserialize an Address Lookup Table account from getAccountInfo bytes.

Never call getAddressLookupTable (not a JSON-RPC method).
"""
from __future__ import annotations

# LOOKUP_TABLE_META_SIZE includes the u32 ProgramState (web3.js / Agave).
LUT_META = 56
LUT_OFF = LUT_META
LUT_NEVER = (1 << 64) - 1
ALT_OWNER = "AddressLookupTab1e1111111111111111111111111"


def parse_account(data: bytes) -> dict | None:
    if data is None or len(data) < LUT_OFF:
        return None
    typ = int.from_bytes(data[0:4], "little")
    if typ != 1:
        return None
    rest = data[LUT_OFF:]
    if len(rest) % 32 != 0:
        return None
    n = len(rest) // 32
    if n > 256:
        return None
    addrs = [rest[i * 32 : (i + 1) * 32] for i in range(n)]
    return {
        "deactivation_slot": int.from_bytes(data[4:12], "little"),
        "last_extended_slot": int.from_bytes(data[12:20], "little"),
        "start_index": data[20],
        "n": len(addrs),
        "addrs": addrs,
    }


def index_blocked(meta: dict, slot: int, idx: int) -> bool:
    deact = meta["deactivation_slot"]
    if deact != LUT_NEVER and slot > deact:
        return True
    if slot == meta["last_extended_slot"] and idx >= meta["start_index"]:
        return True
    return False
