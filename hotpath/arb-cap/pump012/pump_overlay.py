"""PUMP-TX-016 — per-CPI tx-local overlay. Does not change the C kernel.

There is no single Pump N per signature/pool.
Each Pump CPI is applied in execution order to a tx-local copy of S.
Unknown relevant CPI → fail closed (not TX_EXACT).
Predicted S' is the terminal overlay. Never N = published vault delta.
"""
from __future__ import annotations

from pump_apply import PUMP_DIR_BASE_TO_QUOTE, PUMP_DIR_QUOTE_TO_BASE, apply_swap
from pump_fee import b58decode_any, flatten_keys, resolve_fee_state

MODEL = "pump-overlay-016"

SELL = bytes.fromhex("33e685a4017f83ad")
BUY_EQ = bytes.fromhex("c62e1552b4d9e870")
BUY_OUT = bytes.fromhex("66063d1201daebea")  # buy: base_amount_out, max_quote_in
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"

KIND = {
    SELL: "sell",
    BUY_EQ: "buy_exact_quote_in",
    BUY_OUT: "buy_exact_out",
}


def apply_buy_exact_out(before: dict, base_out: int, max_quote: int | None = None) -> dict | None:
    """Exact-out buy: ix.amount is base leaving the pool. Invert quote-in via apply_swap."""
    if base_out <= 0:
        return None
    rb = int(before.get("base_vault_amount", before["reserve_base"]))
    want = rb - base_out
    if want <= 0:
        return None
    lo, hi = 1, (max_quote or 10**18)
    ans = None
    while lo <= hi:
        mid = (lo + hi) // 2
        got = apply_swap(before, mid, PUMP_DIR_QUOTE_TO_BASE)
        if got is None:
            hi = mid - 1
            continue
        if got["reserve_base"] == want:
            ans = mid
            break
        if got["reserve_base"] > want:
            lo = mid + 1
        else:
            hi = mid - 1
    if ans is None:
        return None
    out = apply_swap(before, ans, PUMP_DIR_QUOTE_TO_BASE)
    if not out or out["reserve_base"] != want:
        return None
    out["amount_in"] = ans
    out["base_amount_out"] = base_out
    out["direction"] = PUMP_DIR_QUOTE_TO_BASE
    return out


def apply_cpi(before: dict, cpi: dict) -> dict | None:
    kind = cpi.get("kind")
    amt = cpi.get("amount")
    if amt is None:
        return None
    if kind == "sell":
        return apply_swap(before, int(amt), PUMP_DIR_BASE_TO_QUOTE)
    if kind == "buy_exact_quote_in":
        return apply_swap(before, int(amt), PUMP_DIR_QUOTE_TO_BASE)
    if kind == "buy_exact_out":
        return apply_buy_exact_out(before, int(amt), cpi.get("max_quote"))
    return None


def cpi_id(sig: str, cpi: dict) -> dict:
    return {
        "sig": sig,
        "outer_ix": cpi.get("outer_ix"),
        "inner_ordinal": cpi.get("inner_ordinal"),
        "pool": cpi.get("pool"),
        "direction": cpi.get("direction"),
        "src_ata": cpi.get("src_ata"),
        "dst_ata": cpi.get("dst_ata"),
        "kind": cpi.get("kind"),
        "model_version": MODEL,
    }


def fee_overlay_state(s_before: dict, fee_cfg: dict | None, gcfg: dict | None,
                      pool: dict | None, supply: int, rb: int, rq: int) -> dict:
    st = dict(s_before)
    if not fee_cfg or not gcfg or not pool:
        return st
    try:
        fs = resolve_fee_state(
            fee_cfg, gcfg, pool, supply, rb, rq,
            gate="coin_creator", noncanon="flat",
        )
    except Exception:
        return st
    st["lp_fee_bps"] = fs["lp_fee_bps"]
    st["protocol_fee_bps"] = fs["protocol_fee_bps"]
    st["creator_fee_bps"] = fs["creator_fee_bps"]
    return st


def apply_sequence(s0: dict, cpis: list[dict]) -> tuple[dict | None, str | None]:
    """Apply known Pump CPIs in order. Unknown kind → fail closed."""
    s = dict(s0)
    if not cpis:
        return None, "association_ambiguity"
    if any(c.get("kind") == "unknown" for c in cpis):
        return None, "unsupported_cpi"
    for c in cpis:
        if c.get("kind") not in KIND.values() and c.get("kind") not in (
            "sell", "buy_exact_quote_in", "buy_exact_out",
        ):
            return None, "unsupported_cpi"
        nxt = apply_cpi(s, c)
        if nxt is None:
            return None, "unsupported_cpi"
        s = nxt
    return s, None


def vaults(s: dict) -> tuple[int, int]:
    return int(s["reserve_base"]), int(s["reserve_quote"])


def _decode_ix(ix: dict, keys: list[str], outer_ix: int, inner_ordinal: int | None) -> dict | None:
    prog = ix.get("programId")
    if prog is None and isinstance(ix.get("programIdIndex"), int):
        pi = ix["programIdIndex"]
        prog = keys[pi] if pi < len(keys) else ""
    if prog != PUMP:
        return None
    data = ix.get("data") or ""
    raw = b""
    if isinstance(data, str) and data:
        try:
            raw = b58decode_any(data)
        except Exception:
            raw = b""
    elif isinstance(data, (bytes, bytearray)):
        raw = bytes(data)
    if len(raw) < 16:
        return {"kind": "unknown", "outer_ix": outer_ix, "inner_ordinal": inner_ordinal}
    disc = raw[:8]
    kind = KIND.get(disc)
    a = int.from_bytes(raw[8:16], "little")
    b = int.from_bytes(raw[16:24], "little") if len(raw) >= 24 else None
    accs = []
    if isinstance(ix.get("accounts"), list) and ix["accounts"] and isinstance(ix["accounts"][0], str):
        accs = list(ix["accounts"])
    else:
        for ai in ix.get("accounts") or []:
            if isinstance(ai, int) and ai < len(keys):
                accs.append(keys[ai])
    if kind is None:
        return {
            "kind": "unknown", "disc": disc.hex(),
            "outer_ix": outer_ix, "inner_ordinal": inner_ordinal,
            "pool": accs[0] if accs else None,
        }
    if kind == "sell":
        direction = PUMP_DIR_BASE_TO_QUOTE
        src, dst = (accs[5] if len(accs) > 5 else None), (accs[7] if len(accs) > 7 else None)
    else:
        direction = PUMP_DIR_QUOTE_TO_BASE
        src, dst = (accs[6] if len(accs) > 6 else None), (accs[8] if len(accs) > 8 else None)
    return {
        "kind": kind,
        "amount": a,
        "max_quote": b if kind == "buy_exact_out" else None,
        "min_out": b if kind != "buy_exact_out" else None,
        "pool": accs[0] if accs else None,
        "direction": direction,
        "src_ata": src,
        "dst_ata": dst,
        "outer_ix": outer_ix,
        "inner_ordinal": inner_ordinal,
        "user": accs[1] if len(accs) > 1 else None,
    }


def walk_cpis(tx: dict) -> list[dict]:
    """Pump CPIs in execution order: for each outer, the outer itself, then its inners."""
    keys = flatten_keys(tx)
    msg = (tx.get("transaction") or {}).get("message") or {}
    outers = msg.get("instructions") or []
    inners_by = {}
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        inners_by.setdefault(int(g.get("index") or 0), []).extend(g.get("instructions") or [])
    out = []
    for i, ox in enumerate(outers):
        d = _decode_ix(ox, keys, i, None)
        if d:
            out.append(d)
        for j, ix in enumerate(inners_by.get(i) or []):
            d = _decode_ix(ix, keys, i, j)
            if d:
                out.append(d)
    return out


def cpis_for_pool(tx: dict, pool: str) -> list[dict]:
    return [c for c in walk_cpis(tx) if c.get("pool") == pool]


def classify_overlay(s_before: dict, published: dict, terminal: dict | None, fail: str | None,
                     cpis: list[dict]) -> str:
    if fail:
        return fail
    if not cpis:
        return "association_ambiguity"
    if any(c.get("kind") == "unknown" for c in cpis):
        return "unsupported_cpi"
    if terminal is None:
        return "unsupported_cpi"
    urb = int(published["reserve_base"])
    urq = int(published["reserve_quote"])
    if terminal["reserve_base"] == urb and terminal["reserve_quote"] == urq:
        return "bit_exact"
    if terminal["reserve_base"] == urb:
        return "missing_state"  # net N is right; quote needs fee/config/V
    return "publication_order"
