"""Bit-identical Python of CORE-006 pump_apply_swap. Does not change the C kernel.

reserve_base / reserve_quote are vault amounts.
Y_effective = quote_vault + virtual_quote is derived only here.
"""
from __future__ import annotations

PUMP_FEE_BPS_DEN = 10000
PUMP_DIR_QUOTE_TO_BASE = 0
PUMP_DIR_BASE_TO_QUOTE = 1


def _ceil_div(n: int, d: int) -> int:
    return n // d + (1 if n % d else 0)


def _fee(n: int, bps: int) -> int:
    return _ceil_div(n * bps, PUMP_FEE_BPS_DEN)


def apply_swap(before: dict, amount_in: int, direction: int) -> dict | None:
    if amount_in <= 0:
        return None
    if int(before.get("status") or 0) != 0:
        return None
    rb = int(before.get("base_vault_amount", before["reserve_base"]))
    rq = int(before.get("quote_vault_amount", before["reserve_quote"]))
    vq = int(before.get("virtual_quote_reserves", before.get("virtual_quote") or 0))
    lp_b = int(before.get("lp_fee_bps") or 0)
    pr_b = int(before.get("protocol_fee_bps") or 0)
    cr_b = int(before.get("creator_fee_bps") or 0)
    if rb == 0 or rq == 0:
        return None
    qeff = rq + vq
    if qeff <= 0:
        return None
    total = lp_b + pr_b + cr_b
    if total >= PUMP_FEE_BPS_DEN:
        return None

    if direction == PUMP_DIR_BASE_TO_QUOTE:
        gross = qeff * amount_in // (rb + amount_in)
        lp, proto, creator = _fee(gross, lp_b), _fee(gross, pr_b), _fee(gross, cr_b)
        fee = lp + proto + creator
        if gross < fee:
            return None
        leave = gross - lp
        if leave > rq:
            return None
        out = gross - fee
        after_rb = rb + amount_in
        after_rq = rq - leave
    elif direction == PUMP_DIR_QUOTE_TO_BASE:
        effective = amount_in * PUMP_FEE_BPS_DEN // (PUMP_FEE_BPS_DEN + total)
        lp, proto, creator = _fee(effective, lp_b), _fee(effective, pr_b), _fee(effective, cr_b)
        fee = lp + proto + creator
        if effective + fee > amount_in:
            effective -= effective + fee - amount_in
            lp, proto, creator = _fee(effective, lp_b), _fee(effective, pr_b), _fee(effective, cr_b)
            fee = lp + proto + creator
        if effective < 1:
            return None
        net = effective - 1
        out = rb * net // (qeff + net)
        if out == 0 or out > rb:
            return None
        after_rb = rb - out
        after_rq = rq + amount_in - proto - creator
    else:
        return None
    if out <= 0:
        return None
    return {
        "kind": "pump",
        "reserve_base": after_rb,
        "reserve_quote": after_rq,
        "virtual_quote": vq,
        "base_vault_amount": after_rb,
        "quote_vault_amount": after_rq,
        "virtual_quote_reserves": vq,
        "amount_in": amount_in,
        "direction": direction,
    }


def invert_virtual(before: dict, amount_in: int, direction: int, published: dict) -> int | None:
    """Find virtual_quote_reserves such that apply(before, V) vaults == published."""
    urb = int(published.get("base_vault_amount", published["reserve_base"]))
    urq = int(published.get("quote_vault_amount", published["reserve_quote"]))
    lo, hi = 0, 10**18
    ans = None
    while lo <= hi:
        mid = (lo + hi) // 2
        trial = dict(before)
        trial["virtual_quote"] = mid
        trial["virtual_quote_reserves"] = mid
        got = apply_swap(trial, amount_in, direction)
        if got is None:
            hi = mid - 1
            continue
        if got["reserve_base"] == urb and got["reserve_quote"] == urq:
            ans = mid
            break
        # sell: larger V → smaller after_quote. buy: larger V → larger after_base.
        if direction == PUMP_DIR_BASE_TO_QUOTE:
            if got["reserve_quote"] > urq:
                lo = mid + 1
            else:
                hi = mid - 1
        else:
            if got["reserve_base"] < urb:
                lo = mid + 1
            else:
                hi = mid - 1
    if ans is None:
        return None
    trial = dict(before)
    trial["virtual_quote"] = trial["virtual_quote_reserves"] = ans
    got = apply_swap(trial, amount_in, direction)
    if not got or got["reserve_base"] != urb or got["reserve_quote"] != urq:
        return None
    return ans
