"""PUMP-FEE-015 — recipient-set debit from FeeConfig + GlobalConfig.

Authoritative sources (pump-public-docs + @pump-fun/pump-swap-sdk):
  pfeeUxB6 FeeConfig PDA 5PHirr8…  (seeds fee_config + Pump AMM program id)
  GlobalConfig protocol/reserved/buyback recipient arrays + buyback_bps
  isPumpPool := pool.creator == PDA(pump, [\"pool-authority\", base_mint])
  Pool.creator_fee_bps == 0 means \"use schedule\", not zero
    (only a positive pool field overrides, and only if creator_fee_configurable)

Does not patch the C kernel. Does not fit a mystery bps.
"""
from __future__ import annotations

import hashlib
import struct

PUMP_AMM = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
PUMP_FUN = "6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P"
PFEE = "pfeeUxB6jkeY1Hxd7CsFCAjcbHA9rWtchMGdZ6VojVZ"
FEE_CONFIG_PK = "5PHirr8joyTMp9JMm6nW7hNDVyEYdkzDqazxPD7RaTjx"
WSOL = "So11111111111111111111111111111111111111112"
USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
USDT = "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"
ZERO32 = bytes(32)
BPS_DEN = 10000

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
_P = 2**255 - 19
_D = 370957059346694393431380882127351390395656022343698204509819366164078637887


def b58decode(s: str) -> bytes:
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    h = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    raw = b"\x00" * pad + h
    return raw[-32:] if len(raw) >= 32 else raw.rjust(32, b"\x00")


def b58decode_any(s: str) -> bytes:
    """Base58 without 32-byte pubkey padding (ix data)."""
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    h = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\x00" * pad + h


def flatten_keys(tx: dict) -> list[str]:
    msg = (tx.get("transaction") or {}).get("message") or {}
    keys = []
    for a in msg.get("accountKeys") or []:
        keys.append(a.get("pubkey") if isinstance(a, dict) else str(a))
    la = (tx.get("meta") or {}).get("loadedAddresses") or {}
    keys.extend(la.get("writable") or [])
    keys.extend(la.get("readonly") or [])
    return keys


def b58encode(raw: bytes) -> str:
    n = int.from_bytes(raw, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return B58[0] * pad + (out or B58[0])


def _on_curve(pk: bytes) -> bool:
    if len(pk) != 32:
        return False
    y = int.from_bytes(pk, "little") & ((1 << 255) - 1)
    y2 = y * y % _P
    u = (y2 - 1) % _P
    v = (_D * y2 + 1) % _P
    x2 = (u * pow(v, _P - 2, _P)) % _P
    return pow(x2, (_P - 1) // 2, _P) != _P - 1


def find_pda(seeds: list[bytes], program: bytes) -> bytes:
    for bump in range(255, -1, -1):
        h = hashlib.sha256()
        for s in seeds:
            h.update(s)
        h.update(bytes([bump]))
        h.update(program)
        h.update(b"ProgramDerivedAddress")
        digest = h.digest()
        if not _on_curve(digest):
            return digest
    raise RuntimeError("no pda")


def pump_pool_authority(base_mint: str) -> str:
    return b58encode(find_pda([b"pool-authority", b58decode(base_mint)], b58decode(PUMP_FUN)))


def is_pump_pool(base_mint: str, pool_creator: str) -> bool:
    return pool_creator == pump_pool_authority(base_mint)


def ceil_div(n: int, d: int) -> int:
    return n // d + (1 if n % d else 0)


def fee_bps(n: int, bps: int) -> int:
    return ceil_div(n * bps, BPS_DEN) if bps else 0


def _pk(raw: bytes, off: int) -> str:
    return b58encode(raw[off:off + 32])


def _u64(raw: bytes, off: int) -> int:
    return struct.unpack_from("<Q", raw, off)[0]


def _u128(raw: bytes, off: int) -> int:
    return int.from_bytes(raw[off:off + 16], "little")


def _fees(raw: bytes, off: int) -> tuple[dict, int]:
    return {
        "lp_fee_bps": _u64(raw, off),
        "protocol_fee_bps": _u64(raw, off + 8),
        "creator_fee_bps": _u64(raw, off + 16),
    }, off + 24


def _vec_tiers(raw: bytes, off: int) -> tuple[list[dict], int]:
    n = struct.unpack_from("<I", raw, off)[0]
    off += 4
    out = []
    for _ in range(n):
        thr = _u128(raw, off)
        fees, off2 = _fees(raw, off + 16)
        off = off2
        out.append({"market_cap_lamports_threshold": thr, "fees": fees})
    return out, off


def parse_fee_config(raw: bytes) -> dict:
    off = 8  # disc
    bump = raw[off]
    off += 1
    admin = _pk(raw, off)
    off += 32
    flat, off = _fees(raw, off)
    tiers, off = _vec_tiers(raw, off)
    stable, off = _vec_tiers(raw, off) if off + 4 <= len(raw) else ([], off)
    exotic = {"lp_fee_bps": 0, "protocol_fee_bps": 0, "creator_fee_bps": 0}
    if off + 24 <= len(raw):
        exotic, off = _fees(raw, off)
    return {
        "bump": bump,
        "admin": admin,
        "flat_fees": flat,
        "fee_tiers": tiers,
        "stable_fee_tiers": stable,
        "exotic_flat_fees": exotic,
        "bytes": len(raw),
        "tail": off,
    }


def _pks(raw: bytes, off: int, n: int) -> tuple[list[str], int]:
    out = [_pk(raw, off + i * 32) for i in range(n)]
    return out, off + n * 32


def parse_global_config(raw: bytes) -> dict:
    off = 8
    admin = _pk(raw, off)
    off += 32
    lp = _u64(raw, off)
    proto = _u64(raw, off + 8)
    off += 16
    disable = raw[off]
    off += 1
    recips, off = _pks(raw, off, 8)
    creator = _u64(raw, off) if off + 8 <= len(raw) else 0
    off += 8
    admin_set = _pk(raw, off) if off + 32 <= len(raw) else ""
    off += 32
    whitelist = _pk(raw, off) if off + 32 <= len(raw) else ""
    off += 32
    reserved = _pk(raw, off) if off + 32 <= len(raw) else ""
    off += 32
    mayhem = bool(raw[off]) if off < len(raw) else False
    off += 1
    reserved_list, off = _pks(raw, off, 7) if off + 7 * 32 <= len(raw) else ([], off)
    cashback = bool(raw[off]) if off < len(raw) else False
    off += 1
    buyback_recips, off = _pks(raw, off, 8) if off + 8 * 32 <= len(raw) else ([], off)
    buyback_bps = _u64(raw, off) if off + 8 <= len(raw) else 0
    off += 8
    boost_auth = _pk(raw, off) if off + 32 <= len(raw) else ""
    off += 32
    boost = bool(raw[off]) if off < len(raw) else False
    off += 1
    cr_cfg = bool(raw[off]) if off < len(raw) else False
    off += 1
    max_cr = _u64(raw, off) if off + 8 <= len(raw) else 0
    return {
        "admin": admin,
        "lp_fee_bps": lp,
        "protocol_fee_bps": proto,
        "disable_flags": disable,
        "protocol_fee_recipients": recips,
        "coin_creator_fee_bps": creator,
        "admin_set_coin_creator_authority": admin_set,
        "whitelist_pda": whitelist,
        "reserved_fee_recipient": reserved,
        "mayhem_mode_enabled": mayhem,
        "reserved_fee_recipients": reserved_list,
        "is_cashback_enabled": cashback,
        "buyback_fee_recipients": buyback_recips,
        "buyback_basis_points": buyback_bps,
        "boost_authority": boost_auth,
        "boost_enabled": boost,
        "creator_fee_configurable": cr_cfg,
        "max_configurable_creator_fee_bps": max_cr,
        "bytes": len(raw),
    }


def parse_pool_meta(raw: bytes) -> dict:
    creator = _pk(raw, 11) if len(raw) >= 43 else ""
    base = _pk(raw, 43) if len(raw) >= 75 else ""
    quote = _pk(raw, 75) if len(raw) >= 107 else ""
    coin_creator = _pk(raw, 211) if len(raw) >= 243 else ""
    cr = int.from_bytes(raw[261:269], "little") if len(raw) >= 269 else 0
    virt = int.from_bytes(raw[245:261], "little", signed=True) if len(raw) >= 261 else 0
    return {
        "creator": creator,
        "base_mint": base,
        "quote_mint": quote,
        "coin_creator": coin_creator,
        "creator_fee_bps": cr,
        "virtual_quote_reserves": virt,
        "mayhem": raw[243] if len(raw) > 243 else 0,
        "cashback": raw[244] if len(raw) > 244 else 0,
        "holder": raw[270] if len(raw) > 270 else 0,
        "canonical": bool(base and creator and is_pump_pool(base, creator)),
    }


def pool_market_cap(base_supply: int, base_reserve: int, quote_reserve: int) -> int:
    if base_reserve <= 0:
        return 0
    return quote_reserve * base_supply // base_reserve


def calculate_fee_tier(tiers: list[dict], market_cap: int) -> dict:
    if not tiers:
        return {"lp_fee_bps": 0, "protocol_fee_bps": 0, "creator_fee_bps": 0}
    first = tiers[0]
    if market_cap < int(first["market_cap_lamports_threshold"]):
        return dict(first["fees"])
    for tier in reversed(tiers):
        if market_cap >= int(tier["market_cap_lamports_threshold"]):
            return dict(tier["fees"])
    return dict(first["fees"])


def _quote_class(quote_mint: str) -> str:
    if quote_mint == WSOL:
        return "sol"
    if quote_mint in (USDC, USDT):
        return "stable"
    return "exotic"


def uses_sol_tiers(pool: dict, gate: str) -> bool:
    """Which Pool field selects FeeConfig.fee_tiers vs Global/flat.

    Documented gate is isPumpPool (pool.creator == pool-authority PDA).
    Idx-399 class fails that gate but pays sol tier 0 — live program
    is using a wider predicate. `gate` names the field under test.
    """
    if gate == "isPumpPool":
        return bool(pool.get("canonical"))
    if gate == "coin_creator":
        cr = pool.get("coin_creator") or ""
        return bool(cr) and cr != b58encode(ZERO32)
    if gate == "virtual":
        return int(pool.get("virtual_quote_reserves") or 0) != 0
    if gate == "sol_quote":
        return (pool.get("quote_mint") or "") == WSOL
    if gate == "always":
        return True
    raise ValueError(gate)


def schedule_fees(fee_cfg: dict, use_tiers: bool, quote_mint: str, market_cap: int) -> dict:
    """pfee get_fees: tiers when use_tiers; else flatFees."""
    q = _quote_class(quote_mint)
    if not use_tiers:
        return dict(fee_cfg["flat_fees"])
    if q == "stable" and fee_cfg.get("stable_fee_tiers"):
        return calculate_fee_tier(fee_cfg["stable_fee_tiers"], market_cap)
    if q == "exotic":
        ex = fee_cfg.get("exotic_flat_fees") or {}
        if any(int(ex.get(k) or 0) for k in ("lp_fee_bps", "protocol_fee_bps", "creator_fee_bps")):
            return dict(ex)
        return dict(fee_cfg["flat_fees"])
    return calculate_fee_tier(fee_cfg["fee_tiers"], market_cap)


def resolve_creator(schedule_cr: int, pool_cr: int, configurable: bool) -> tuple[int, str]:
    if configurable and pool_cr > 0:
        return pool_cr, "pool"
    return schedule_cr, "schedule"


def resolve_fee_state(
    fee_cfg: dict,
    gcfg: dict,
    pool: dict,
    base_supply: int,
    base_reserve: int,
    quote_reserve: int,
    quote_for_mc: int | None = None,
    gate: str = "isPumpPool",
    noncanon: str = "flat",
) -> dict:
    """Inputs AUTH should carry. quote_for_mc defaults to quote_reserve (vault).

    noncanon: 'flat' uses FeeConfig.flatFees; 'global' uses GlobalConfig lp/proto.
    """
    qmc = quote_reserve if quote_for_mc is None else quote_for_mc
    mc = pool_market_cap(base_supply, base_reserve, qmc)
    canonical = bool(pool.get("canonical"))
    if not canonical and pool.get("creator") and pool.get("base_mint"):
        canonical = is_pump_pool(pool["base_mint"], pool["creator"])
        pool = dict(pool)
        pool["canonical"] = canonical
    use_tiers = uses_sol_tiers(pool, gate)
    if use_tiers:
        sched = schedule_fees(fee_cfg, True, pool.get("quote_mint") or WSOL, mc)
        src = "tier"
    elif noncanon == "global":
        sched = {
            "lp_fee_bps": int(gcfg["lp_fee_bps"]),
            "protocol_fee_bps": int(gcfg["protocol_fee_bps"]),
            "creator_fee_bps": int(gcfg.get("coin_creator_fee_bps") or 0),
        }
        src = "global"
    else:
        sched = schedule_fees(fee_cfg, False, pool.get("quote_mint") or WSOL, mc)
        src = "flat"
    cr, cr_src = resolve_creator(
        int(sched["creator_fee_bps"]),
        int(pool.get("creator_fee_bps") or 0),
        bool(gcfg.get("creator_fee_configurable")),
    )
    return {
        "canonical": canonical,
        "use_tiers": use_tiers,
        "fee_src": src,
        "gate": gate,
        "market_cap": mc,
        "quote_class": _quote_class(pool.get("quote_mint") or WSOL),
        "lp_fee_bps": int(sched["lp_fee_bps"]),
        "protocol_fee_bps": int(sched["protocol_fee_bps"]),
        "creator_fee_bps": cr,
        "creator_source": cr_src,
        "schedule_creator_fee_bps": int(sched["creator_fee_bps"]),
        "buyback_bps": int(gcfg.get("buyback_basis_points") or 0),
        "protocol_fee_recipients": list(gcfg.get("protocol_fee_recipients") or []),
        "buyback_fee_recipients": list(gcfg.get("buyback_fee_recipients") or []),
        "reserved_fee_recipient": gcfg.get("reserved_fee_recipient") or "",
        "reserved_fee_recipients": list(gcfg.get("reserved_fee_recipients") or []),
    }


def buy_components(spendable: int, lp: int, proto: int, creator: int, buyback: int, mode: str) -> dict:
    """Exact integer rounding per component. LP stays in the vault.

    modes:
      add     — buyback is a fourth fee in the total (leaves vault)
      carve   — buyback carved from protocol amount; vault -= proto+creator only
      ignore  — buyback unused
    """
    if mode == "add":
        tot = lp + proto + creator + buyback
    else:
        tot = lp + proto + creator
    if tot >= BPS_DEN:
        return {"ok": False}
    eff = spendable * BPS_DEN // (BPS_DEN + tot)
    lp_a = fee_bps(eff, lp)
    proto_a = fee_bps(eff, proto)
    cr_a = fee_bps(eff, creator)
    if mode == "add":
        bb_a = fee_bps(eff, buyback)
        vault = spendable - proto_a - cr_a - bb_a
    elif mode == "carve":
        bb_a = fee_bps(proto_a, buyback) if buyback else 0
        proto_a_net = proto_a - bb_a
        vault = spendable - proto_a - cr_a
        return {
            "ok": True, "effective": eff, "lp": lp_a,
            "protocol": proto_a_net, "creator": cr_a, "buyback": bb_a,
            "protocol_gross": proto_a,
            "vault": vault, "total_bps": tot, "mode": mode,
        }
    else:
        bb_a = 0
        vault = spendable - proto_a - cr_a
    return {
        "ok": True, "effective": eff, "lp": lp_a,
        "protocol": proto_a, "creator": cr_a, "buyback": bb_a,
        "vault": vault, "total_bps": tot, "mode": mode,
    }


def role_owner(owner: str, gcfg: dict) -> str:
    if owner in (gcfg.get("protocol_fee_recipients") or []):
        return "protocol_fee_recipient"
    if owner in (gcfg.get("buyback_fee_recipients") or []):
        return "buyback_fee_recipient"
    if owner == (gcfg.get("reserved_fee_recipient") or ""):
        return "reserved_fee_recipient"
    if owner in (gcfg.get("reserved_fee_recipients") or []):
        return "reserved_fee_recipient"
    return "other"
