"""Pump compact layout. Proven offsets from live001 / PUMPSTATE012."""
from __future__ import annotations

import struct

PUMP_AMM = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"


def parse_pump_pool(data: bytes) -> dict | None:
    if len(data) < 211:
        return None
    virtual = 0
    if len(data) >= 261:
        virtual = int.from_bytes(data[245:261], "little", signed=True)
        if virtual > (2**63 - 1) or virtual < -(2**63):
            return None
    return {
        "vault_base": data[139:171],
        "vault_quote": data[171:203],
        "virtual_quote": int(virtual),
        "creator_fee_bps": int.from_bytes(data[261:269], "little")
        if len(data) >= 269
        else 0,
    }


def token_amount(data: bytes) -> int | None:
    if not data or len(data) < 72:
        return None
    return int.from_bytes(data[64:72], "little")


def parse_global_config(data: bytes) -> tuple[int, int, int, int]:
    """lp, protocol, creator, disable_flags. Same offsets as live001.parse_global."""
    if len(data) < 57:
        return 20, 5, 0, 0
    lp = struct.unpack_from("<Q", data, 40)[0]
    proto = struct.unpack_from("<Q", data, 48)[0]
    disabled = data[56]
    creator = 0
    if len(data) >= 57 + 32 * 8 + 8:
        creator = struct.unpack_from("<Q", data, 57 + 32 * 8)[0]
    if lp > 10000 or proto > 10000 or creator > 10000:
        return 20, 5, 0, int(disabled)
    return int(lp), int(proto), int(creator), int(disabled)
