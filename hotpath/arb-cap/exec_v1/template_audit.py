#!/usr/bin/env python3
"""Read-only audit of resident route0 v0 templates.

Decodes templates, checks the current bank, and runs simulateTransaction
with sigVerify false and replaceRecentBlockhash. Does not sign with a
wallet, transfer SOL, create ATAs or lookup tables, publish a plane, or send.
"""
from __future__ import annotations

import argparse
import base64
import json
import struct
import sys
from collections import Counter
from pathlib import Path

OUR_EXEC = "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K"
DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
SOL = "So11111111111111111111111111111111111111112"
TOKENKEG = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKEN2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
SYSTEM = "11111111111111111111111111111111"
ATA_PROG = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
MEMO = "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr"
FEE_PROG = "pfeeUxB6jkeY1Hxd7CsFCAjcbHA9rWtchMGdZ6VojVZ"
CU_PROG = "ComputeBudget111111111111111111111111111111"
ALT_PROG = "AddressLookupTab1e1111111111111111111111111"
UPGRADEABLE = "BPFLoaderUpgradeab1e11111111111111111111111"
DISC = b"ARBEXEC0"
HOPS_DISC = b"ARBHOPS0"
FEE_RECIPIENTS = {
    "5YxQFdt3Tr9zJLvkFccqXVUwhdTWJQc1fFg2YPbxvxeD",
    "9M4giFFMxmFGXtc3feFzRai56WbBqehoSeRE5GK7gf7",
    "GXPFM2caqTtQYC2cJ5yJRi9VDkpsYZXzYdwYpGnLmtDL",
    "3BpXnfJaUTiwXnJNe7Ej1rcbzqTTQUvLShZaWazebsVR",
    "5cjcW9wExnJJiqgLjq7DEG75Pm6JBgE1hNv4B2vHXUW6",
    "EHAAiTxcdDwQ3U4bU6YcMsQGaekdzLS3B5SmYo46kJtL",
    "5eHhjP8JaYkz83CWwvGU2uMUXefd3AazWGx4gpcuEEYD",
    "A7hAgCzFw14fejgCp387JUJRMNyz4j89JKnhtKU8piqW",
}
STATIC_EXPECT = [
    None, OUR_EXEC, CU_PROG, TOKENKEG, SYSTEM, ATA_PROG, MEMO,
    DLMM, PUMP, TOKEN2022, FEE_PROG,
]
WR_NAMES = [
    "user_quote", "user_base", "dlmm", "vault_x", "vault_y", "oracle",
    "bin0", "bin1", "pump", "vault_base", "vault_quote", "proto_fee_ata",
    "creator_ata", "user_vol", "fee_rec_quote",
]
WR_NAMES_BUY = WR_NAMES + ["global_vol"]
RO_NAMES = [
    "dlmm_event", "mint_x", "mint_y", "pump_event", "pump_global", "fee_cfg",
    "proto_fee", "creator_auth", "pool_v2", "fee_recipient",
]
_B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58encode(raw: bytes) -> str:
    n = int.from_bytes(raw, "big")
    chars: list[str] = []
    while n:
        n, rem = divmod(n, 58)
        chars.append(_B58[rem])
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    body = "".join(reversed(chars)) if chars else ""
    return ("1" * pad) + body


def _u16(buf: bytes, off: int) -> tuple[int, int]:
    n = buf[off]
    if n < 128:
        return n, off + 1
    return (n & 0x7F) | (buf[off + 1] << 7), off + 2


def parse_template(raw: bytes) -> dict:
    """Decode a compiled route0 v0 template. No RPC."""
    if len(raw) < 70 or raw[0] != 1 or raw[65] != 0x80:
        raise ValueError(f"not a v0 template len={len(raw)}")
    n_static = raw[69]
    off = 70
    static = []
    for _ in range(n_static):
        static.append(b58encode(raw[off:off + 32]))
        off += 32
    off += 32  # blockhash slot
    n_ix, off = _u16(raw, off)
    ixs = []
    for _ in range(n_ix):
        prog = raw[off]
        off += 1
        nacc, off = _u16(raw, off)
        accs = list(raw[off:off + nacc])
        off += nacc
        dlen, off = _u16(raw, off)
        data = raw[off:off + dlen]
        off += dlen
        ixs.append({"prog": prog, "accs": accs, "data": data})
    n_look, off = _u16(raw, off)
    if n_look != 1:
        raise ValueError(f"lookups {n_look}")
    alt = b58encode(raw[off:off + 32])
    off += 32
    nwr, off = _u16(raw, off)
    wr_ix = list(raw[off:off + nwr])
    off += nwr
    nro, off = _u16(raw, off)
    ro_ix = list(raw[off:off + nro])
    off += nro
    if off != len(raw):
        raise ValueError(f"parsed {off} != len {len(raw)}")
    arb = next((ix for ix in ixs if ix["data"][:8] == DISC), None)
    cu_ix = next((ix for ix in ixs if ix["data"][:1] == b"\x02"), None)
    cu_limit = struct.unpack_from("<I", cu_ix["data"], 1)[0] if cu_ix and len(cu_ix["data"]) >= 5 else None
    return {
        "len": len(raw),
        "static": static,
        "alt": alt,
        "wr_ix": wr_ix,
        "ro_ix": ro_ix,
        "disc": arb["data"][:8] if arb else b"",
        "n_ix_acc": len(arb["accs"]) if arb else 0,
        "cu_limit": cu_limit,
        "direction": arb["data"][8] if arb and len(arb["data"]) > 8 else None,
    }


def template_problems(parsed: dict, buy: bool) -> str | None:
    static = parsed["static"]
    if len(static) != 11:
        return f"static_n={len(static)}"
    for i, expect in enumerate(STATIC_EXPECT):
        if expect is not None and static[i] != expect:
            return f"static[{i}]={static[i][:8]}"
    if parsed["disc"] != DISC:
        return "disc"
    want_wr = 16 if buy else 15
    if len(parsed["wr_ix"]) != want_wr or len(parsed["ro_ix"]) != 10:
        return f"lookup wr={len(parsed['wr_ix'])} ro={len(parsed['ro_ix'])}"
    if parsed["cu_limit"] != 400_000:
        return f"cu_limit={parsed['cu_limit']}"
    want_len = 626 if buy else 623
    if parsed["len"] != want_len:
        return f"len={parsed['len']}"
    if parsed["len"] > 1232:
        return "oversize"
    return None


def resolve_keys(parsed: dict, addresses: list[str]) -> tuple[list[str], list[str], str | None]:
    wr, ro = [], []
    for i in parsed["wr_ix"]:
        if i >= len(addresses):
            return [], [], f"wr_index_{i}_of_{len(addresses)}"
        wr.append(addresses[i])
    for i in parsed["ro_ix"]:
        if i >= len(addresses):
            return [], [], f"ro_index_{i}_of_{len(addresses)}"
        ro.append(addresses[i])
    return wr, ro, None


def alt_addresses_from_data(data: bytes) -> tuple[list[str], str | None]:
    if len(data) < 56:
        return [], "alt_short"
    # Current tables store a u32 tag, then the deactivation slot.
    deact = int.from_bytes(data[4:12], "little")
    if deact != 2**64 - 1 and int.from_bytes(data[:8], "little") != 2**64 - 1:
        return [], "alt_deactivated"
    addrs = []
    for i in range(56, len(data), 32):
        if i + 32 > len(data):
            break
        addrs.append(b58encode(data[i:i + 32]))
    return addrs, None


def _custom6(err) -> bool:
    if isinstance(err, dict):
        ie = err.get("InstructionError")
        if isinstance(ie, list) and len(ie) >= 2 and ie[1] == {"Custom": 6}:
            return True
    return False


def sim_class(err) -> tuple[bool, str]:
    if err is None:
        return True, "success"
    if _custom6(err):
        return True, "custom6"
    return False, str(err)[:180]


def first_reason(flags: dict[str, tuple[bool, str]]) -> str:
    for name in ("TEMPLATE_VALID", "ALT_VALID", "VECTOR_VALID", "ATA_VALID", "SIM_VALID"):
        ok, why = flags[name]
        if not ok:
            return f"{name}:{why}"
    return "ok"


def classify(flags: dict[str, tuple[bool, str]]) -> dict:
    out = {}
    for name, (ok, why) in flags.items():
        out[name] = bool(ok)
        out[name + "_reason"] = "" if ok else why
    out["RACE_READY"] = all(ok for ok, _ in flags.values())
    out["reason"] = first_reason(flags)
    if not out["RACE_READY"]:
        out["RACE_READY_reason"] = out["reason"]
    else:
        out["RACE_READY_reason"] = ""
    return out


def _pk(raw: bytes) -> str:
    return b58encode(raw)


def ata_reason(accs: dict, w: dict, mint_x: str, wallet: str | None, dlmm_mod) -> str | None:
    quote = accs.get(w["user_quote"])
    base = accs.get(w["user_base"])
    fee_q = accs.get(w["fee_rec_quote"])
    if not quote or len(quote.get("data") or b"") < 64:
        return "user_quote_missing"
    if quote["data"][0:32] != dlmm_mod.b58decode(SOL) or (
        wallet and quote["data"][32:64] != dlmm_mod.b58decode(wallet)
    ):
        return "user_quote_mint_or_owner"
    if not base or len(base.get("data") or b"") < 64:
        return "user_base_missing"
    if base["data"][0:32] != dlmm_mod.b58decode(mint_x) or (
        wallet and base["data"][32:64] != dlmm_mod.b58decode(wallet)
    ):
        return "user_base_mint_or_owner"
    if not fee_q or len(fee_q.get("data") or b"") < 32:
        return "fee_recipient_quote_missing"
    if fee_q["data"][0:32] != dlmm_mod.b58decode(SOL):
        return "fee_recipient_quote_mint"
    return None


def vector_and_ata(route: dict, wr: list[str], ro: list[str], buy: bool,
                   accs: dict, dlmm_mod) -> tuple[str | None, str | None]:
    """Return (vector_reason, ata_reason). None means that check passed."""
    names = WR_NAMES_BUY if buy else WR_NAMES
    if len(wr) != len(names) or len(ro) != len(RO_NAMES):
        return f"resolved wr={len(wr)} ro={len(ro)}", "not_checked"
    w = dict(zip(names, wr))
    r = dict(zip(RO_NAMES, ro))
    if w["dlmm"] != route.get("dlmm") or w["pump"] != route.get("pump"):
        return "pool_pubkey_mismatch", "not_checked"
    dacc = accs.get(w["dlmm"])
    pacc = accs.get(w["pump"])
    if not dacc or dacc.get("owner") != DLMM:
        return "dlmm_owner", "not_checked"
    if not pacc or pacc.get("owner") != PUMP:
        return "pump_owner", "not_checked"
    try:
        lb = dlmm_mod.parse_lbpair(dacc["data"])
    except Exception as exc:
        return f"lb_parse:{exc}"[:80], "not_checked"
    mint_x = _pk(lb["token_x"])
    mint_y = _pk(lb["token_y"])
    if mint_y != SOL or r["mint_y"] != SOL or r["mint_x"] != mint_x:
        return "mint_mismatch", "not_checked"
    if w["vault_x"] != _pk(lb["vault_x"]) or w["vault_y"] != _pk(lb["vault_y"]):
        return "dlmm_vault_mismatch", "not_checked"
    if route.get("token") and route["token"] != mint_x:
        return "plane_token_mismatch", "not_checked"
    active = dlmm_mod.bin_array_pda(w["dlmm"], dlmm_mod.bin_array_index(int(lb["active_id"])))
    if active not in (w["bin0"], w["bin1"]):
        return "stale_active_bin", ata_reason(accs, w, mint_x, route.get("_wallet"), dlmm_mod)
    for slot in ("bin0", "bin1"):
        bacc = accs.get(w[slot])
        if not bacc or bacc.get("owner") != DLMM:
            return f"{slot}_missing", "not_checked"
    oracle = _pk(dlmm_mod.find_pda(
        [b"oracle", dlmm_mod.b58decode(w["dlmm"])], dlmm_mod.b58decode(DLMM),
    ))
    if w["oracle"] != oracle or r["dlmm_event"] != _pk(dlmm_mod.find_pda(
        [b"__event_authority"], dlmm_mod.b58decode(DLMM),
    )):
        return "dlmm_pda_mismatch", "not_checked"
    try:
        import live001 as live
        pool = live.parse_pump_pool(pacc["data"])
    except Exception as exc:
        return f"pump_parse:{exc}"[:80], "not_checked"
    if not pool:
        return "pump_parse", "not_checked"
    if w["vault_base"] != _pk(pool["vault_base"]) or w["vault_quote"] != _pk(pool["vault_quote"]):
        return "pump_vault_mismatch", "not_checked"
    if _pk(pool["base"]) != mint_x or _pk(pool["quote"]) != SOL:
        return "pump_mint_mismatch", "not_checked"
    coin = pool.get("coin_creator") or b""
    if coin and coin != bytes(32):
        auth = _pk(dlmm_mod.find_pda([b"creator_vault", coin], dlmm_mod.b58decode(PUMP)))
        if r["creator_auth"] != auth:
            return "creator_auth_mismatch", "not_checked"
    if r["fee_recipient"] not in FEE_RECIPIENTS:
        return "fee_recipient", "not_checked"
    mint_acc = accs.get(mint_x) or accs.get(r["mint_x"])
    mint_owner = (mint_acc or {}).get("owner")
    if mint_owner and mint_owner not in (TOKENKEG, TOKEN2022):
        return f"mint_token_program:{mint_owner[:8]}", "not_checked"
    ub = accs.get(w["user_base"])
    if mint_owner and ub and ub.get("owner") and ub["owner"] != mint_owner:
        return f"user_base_token_program:{ub['owner'][:8]}!={mint_owner[:8]}", ata_reason(
            accs, w, mint_x, route.get("_wallet"), dlmm_mod,
        )
    return None, ata_reason(accs, w, mint_x, route.get("_wallet"), dlmm_mod)


def patch_for_sim(raw: bytes, direction: int, amount: int, min_profit: int, buy: bool) -> bytes:
    out = bytearray(raw)
    out[1:65] = bytes(64)
    out[467:475] = struct.pack("<Q", 800_000)
    if buy:
        out[548] = direction
        out[549:557] = struct.pack("<Q", amount)
        out[557:565] = struct.pack("<Q", min_profit)
    else:
        out[546] = direction
        out[547:555] = struct.pack("<Q", amount)
        out[555:563] = struct.pack("<Q", min_profit)
    return bytes(out)


def _sim(rpc, tx: bytes) -> dict:
    blob = base64.b64encode(tx).decode()
    res = rpc("simulateTransaction", [blob, {
        "sigVerify": False,
        "replaceRecentBlockhash": True,
        "commitment": "processed",
        "encoding": "base64",
    }], retries=4, backoff=1.0)
    value = res.get("value") or {}
    ok, why = sim_class(value.get("err"))
    logs = value.get("logs") or []
    interesting = [ln for ln in logs if "Error" in ln or "failed" in ln or "Custom" in ln]
    return {
        "ok": ok,
        "why": why,
        "cu": value.get("unitsConsumed"),
        "raw": len(tx),
        "log": (interesting[-1] if interesting else (logs[-1] if logs else ""))[:180],
    }


def _program_info(get_multiple, pubkey: str, disc: bytes) -> dict:
    acc = get_multiple([pubkey])[0]
    info = {"pubkey": pubkey, "exists": bool(acc), "executable": False, "upgradeable": False, "disc_in_programdata": False}
    if not acc:
        info["reason"] = "missing"
        return info
    info["executable"] = bool(acc.get("executable"))
    info["upgradeable"] = acc.get("owner") == UPGRADEABLE
    info["lamports"] = acc.get("lamports")
    data = acc.get("data") or b""
    if len(data) >= 36:
        progdata = b58encode(data[4:36])
        body = get_multiple([progdata])[0]
        raw = (body or {}).get("data") or b""
        info["programdata"] = progdata
        info["programdata_len"] = len(raw)
        info["disc_in_programdata"] = disc in raw
    # An 8-byte discriminator often compiles to an immediate, so a missing
    # ASCII string is not by itself a different program.
    info["reason"] = "" if info["executable"] and info["upgradeable"] else "program_mismatch"
    return info


def audit_routes(plane: dict, get_multiple, rpc, dlmm_mod, limit: int | None = None,
                 pools: list[str] | None = None) -> list[dict]:
    alts = {a["pubkey"]: a.get("addresses") or [] for a in plane.get("alts") or [] if a.get("pubkey")}
    routes = [r for r in (plane.get("routes") or []) if r.get("tmpl0") and r.get("tmpl1")]
    if pools:
        want = set(pools)
        routes = [r for r in routes if r.get("dlmm") in want or r.get("pump") in want]
    if limit:
        routes = routes[:limit]
    need = []
    parsed_rows = []
    for route in routes:
        item = {"dlmm": route.get("dlmm"), "pump": route.get("pump"), "token": route.get("token")}
        try:
            p0 = parse_template(bytes.fromhex(route["tmpl0"]))
            p1 = parse_template(bytes.fromhex(route["tmpl1"]))
        except Exception as exc:
            item["parsed"] = None
            item["template_error"] = str(exc)[:120]
            parsed_rows.append((route, item, None, None))
            continue
        item["wallet"] = p0["static"][0]
        item["parsed"] = True
        for pk in (p0["alt"], p1["alt"], route.get("dlmm"), route.get("pump")):
            if pk:
                need.append(pk)
        parsed_rows.append((route, item, p0, p1))
    chain_alts = {}
    accs: dict = {}
    fetched = get_multiple([p for p in dict.fromkeys(need) if p])
    for pk, acc in zip([p for p in dict.fromkeys(need) if p], fetched):
        accs[pk] = acc
        if acc and acc.get("owner") == ALT_PROG:
            addrs, why = alt_addresses_from_data(acc["data"])
            chain_alts[pk] = (addrs, why)
    # second wave: accounts named by resolved keys
    more = []
    resolved = []
    for route, item, p0, p1 in parsed_rows:
        if not p0:
            resolved.append((route, item, None))
            continue
        pair = []
        alt_reason = None
        for parsed, buy in ((p0, False), (p1, True)):
            chain, why = chain_alts.get(parsed["alt"], ([], "alt_missing"))
            file_addrs = alts.get(parsed["alt"]) or []
            if why:
                alt_reason = why
            elif file_addrs and file_addrs != chain:
                alt_reason = "alt_file_mismatch"
            wr, ro, err = resolve_keys(parsed, chain)
            if err:
                alt_reason = err
            pair.append((parsed, buy, wr, ro))
            more.extend(wr)
            more.extend(ro)
        item["_alt_reason"] = alt_reason
        resolved.append((route, item, pair))
    more_u = [p for p in dict.fromkeys(more) if p and p not in accs]
    got = get_multiple(more_u) if more_u else []
    for pk, acc in zip(more_u, got):
        accs[pk] = acc
    out = []
    for route, item, pair in resolved:
        if not pair:
            flags = {
                "TEMPLATE_VALID": (False, item.get("template_error") or "parse"),
                "ALT_VALID": (False, "blocked_by_template"),
                "VECTOR_VALID": (False, "blocked_by_template"),
                "ATA_VALID": (False, "blocked_by_template"),
                "SIM_VALID": (False, "blocked_by_template"),
            }
            row = {**item, **classify(flags), "dirs": []}
            out.append(row)
            continue
        t_reasons = []
        for parsed, buy, _wr, _ro in pair:
            why = template_problems(parsed, buy)
            if why:
                t_reasons.append(why)
        route = dict(route)
        route["_wallet"] = item.get("wallet")
        v_reason = a_reason = None
        if not t_reasons and not item.get("_alt_reason"):
            for _parsed, buy, wr, ro in pair:
                try:
                    vr, ar = vector_and_ata(route, wr, ro, buy, accs, dlmm_mod)
                except Exception as exc:
                    vr, ar = f"vector_exc:{type(exc).__name__}"[:80], "not_checked"
                if vr and not v_reason:
                    v_reason = vr
                if ar and not a_reason:
                    a_reason = ar
        dirs = []
        sim_reason = None
        if not t_reasons:
            for parsed, buy, _wr, _ro in pair:
                raw = bytes.fromhex(route["tmpl1" if buy else "tmpl0"])
                tx = patch_for_sim(raw, 1 if buy else 0, 50_000_000, 525_000, buy)
                try:
                    sim = _sim(rpc, tx)
                except Exception as exc:
                    sim = {"ok": False, "why": f"rpc:{type(exc).__name__}", "cu": None, "raw": len(tx)}
                sim["dir"] = 1 if buy else 0
                sim["cu_limit"] = parsed["cu_limit"]
                dirs.append(sim)
                if not sim["ok"] and not sim_reason:
                    sim_reason = f"dir{sim['dir']}:{sim['why']}"
        flags = {
            "TEMPLATE_VALID": (not t_reasons, t_reasons[0] if t_reasons else ""),
            "ALT_VALID": (not item.get("_alt_reason"), item.get("_alt_reason") or ""),
            "VECTOR_VALID": (v_reason is None and not t_reasons and not item.get("_alt_reason"), v_reason or ("blocked" if t_reasons or item.get("_alt_reason") else "")),
            "ATA_VALID": (a_reason is None and v_reason is None and not t_reasons and not item.get("_alt_reason"), a_reason or ("blocked" if (v_reason or t_reasons or item.get("_alt_reason")) else "")),
            "SIM_VALID": (sim_reason is None and not t_reasons and bool(dirs), sim_reason or "blocked"),
        }
        # VECTOR/ATA/SIM reasons when earlier failed
        if t_reasons or item.get("_alt_reason"):
            flags["VECTOR_VALID"] = (False, "blocked")
            flags["ATA_VALID"] = (False, "blocked")
        elif v_reason:
            if a_reason is None:
                flags["ATA_VALID"] = (True, "")
            else:
                flags["ATA_VALID"] = (False, a_reason)
        clean = {k: v for k, v in item.items() if not k.startswith("_") and k != "parsed"}
        row = {**clean, **classify(flags), "dirs": dirs}
        out.append(row)
        print(
            f"  {row.get('dlmm','')[:8]} {row['reason']} "
            f"cu={[d.get('cu') for d in dirs]}",
            file=sys.stderr,
            flush=True,
        )
    return out


def render_report(doc: dict) -> str:
    routes = doc.get("routes") or []
    counts = Counter(r.get("reason", "").split(":")[0] for r in routes)
    n = len(routes)
    ready = sum(1 for r in routes if r.get("RACE_READY"))
    lines = [
        "# Execution readiness",
        "",
        "Read-only. No SOL was moved, no ATA or lookup table was created, no plane was published, and nothing was sent.",
        "",
        f"Route0 templates audited: **{n}**. Report `RACE_READY` (template + alt + vector + ata + unsigned sim): **{ready}**.",
        "",
        "The plane's uppercase `RACE_READY` means both directions compiled and the ATAs existed when the plane was written. This report does not treat that flag as a Custom(6) proof.",
        "",
        "## Coverage",
        "",
        "| check | pass |",
        "|---|---|",
    ]
    for name in ("TEMPLATE_VALID", "ALT_VALID", "VECTOR_VALID", "ATA_VALID", "SIM_VALID", "RACE_READY"):
        lines.append(f"| {name} | {sum(1 for r in routes if r.get(name))} / {n} |")
    lines += ["", "## Failure reasons", ""]
    for reason, c in counts.most_common():
        lines.append(f"- `{reason}` {c}")
    lines += ["", "## CU, size, and the fee the wire bills", ""]
    lines.append(
        "Every template requests CU limit 400000. Oneshot patches CU price 800000. "
        "The fee is signature 5000 + limit*price/1e6 + SWQOS 150000 + safety 50000 "
        f"= **{doc.get('wire_hurdle')}** lamports on every route0 send. "
        "Consumed CU from simulation does not change that bill. "
        "The hurdle was not lowered to consumed CU, because the template still requests 400000."
    )
    lines.append("")
    lines.append("| dir | n | cu min | cu p50 | cu max | raw |")
    lines.append("|---|---|---|---|---|---|")
    for direction in (0, 1):
        cus = sorted(int(d["cu"]) for r in routes for d in r.get("dirs") or [] if d.get("dir") == direction and d.get("cu"))
        raws = [d.get("raw") for r in routes for d in r.get("dirs") or [] if d.get("dir") == direction]
        if not cus:
            lines.append(f"| {direction} | 0 | | | | |")
            continue
        lines.append(
            f"| {direction} | {len(cus)} | {cus[0]} | {cus[len(cus)//2]} | {cus[-1]} | {raws[0] if raws else ''} |"
        )
    ours = doc.get("our_exec") or {}
    hops = doc.get("hops") or {}
    lines += [
        "",
        "## Programs",
        "",
        f"- OUR_EXEC `{ours.get('pubkey')}` executable={ours.get('executable')} "
        f"upgradeable={ours.get('upgradeable')} ARBEXEC0_in_programdata={ours.get('disc_in_programdata')} "
        f"reason=`{ours.get('reason') or 'ok'}`",
        f"- ARBHOPS0 id `{hops.get('pubkey')}` executable={hops.get('executable')} "
        f"upgradeable={hops.get('upgradeable')} disc_in_programdata={hops.get('disc_in_programdata')} "
        f"so_has_disc={hops.get('so_has_disc')} so_bytes={hops.get('so_bytes')} reason=`{hops.get('reason') or 'ok'}`",
        "",
        "## Gate bypasses",
        "",
        "- `FUNDED` defaults to 0 and returns before the signer, the racer, and RPC.",
        "- STATE-008 READY is the only path that can arm. `AUTH_READY` pointing at state007 or any other file is refused.",
        "- Journal `race_ready` (`tx_exact`) and plane `RACE_READY` are both required. They are not OR'd.",
        "- The send floor is the wired 525000, not the paper per-sequence hurdle.",
        "- Family 6 (`dlmm-dlmm`) and any seq other than `dlmm-pump` / `pump-dlmm` are `not_v1_executable`. A shared pubkey no longer looks like a route0 send.",
        "",
        "## Remaining blockers before one funded shot",
        "",
    ]
    blockers = doc.get("blockers") or []
    if not blockers:
        lines.append("- None recorded.")
    for b in blockers:
        lines.append(f"- {b}")
    lines += [
        "",
        "## One shot",
        "",
        "All of these must be true, and a human must say so in that request:",
        "",
        "1. `prearm_check.py` exits 0.",
        "2. `/home/louis/captures/state008/READY` exists because the AUTH writer is coherent. state007 READY does not count.",
        "3. At least one `dlmm-pump` or `pump-dlmm` route in this report is `RACE_READY`.",
        "4. `FUNDED=1` for one `oneshot_live.py` process, then back to 0. No retry.",
        "",
    ]
    return "\n".join(lines) + "\n"


def _hops_info(get_multiple) -> dict:
    id_path = Path("/home/louis/arb-exec/.deploy/hops-program-id.txt")
    so_path = Path("/home/louis/arb-exec-live/program_hops/target/deploy/hops.so")
    pubkey = id_path.read_text(encoding="utf-8").strip() if id_path.exists() else ""
    info = _program_info(get_multiple, pubkey, HOPS_DISC) if pubkey else {"pubkey": "", "reason": "no_id_file"}
    if so_path.exists():
        raw = so_path.read_bytes()
        info["so_bytes"] = len(raw)
        info["so_has_disc"] = HOPS_DISC in raw
    else:
        info["so_bytes"] = 0
        info["so_has_disc"] = False
        info["reason"] = info.get("reason") or "so_missing"
    if info.get("disc_in_programdata") and not info.get("so_has_disc"):
        info["reason"] = "deployed_disc_but_local_so_mismatch"
    return info


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="read-only route0 template audit")
    ap.add_argument("--plane", type=Path, default=Path("/home/louis/arb-exec/.deploy/alt_plane.json"))
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument(
        "--pool", action="append", default=[],
        help="Only these DLMM or Pump pubkeys (repeatable). Unsigned sim only.",
    )
    ap.add_argument(
        "--auth-cap", type=Path, default=None,
        help="Live STATE-008 cap. Default STATE008_CAP or the Pump016 soak.",
    )
    args = ap.parse_args(argv)
    here = Path(__file__).resolve().parent
    for extra in ("/home/louis/arb-cap", "/home/louis/arb-exec/scripts", str(here.parents[1] / "arb-cap")):
        if extra not in sys.path and Path(extra).exists():
            sys.path.insert(0, extra)
    import live001 as live
    import record_dlmm as dlmm
    live.load_dotenv()
    plane = json.loads(args.plane.read_text(encoding="utf-8"))
    print(f"routes={len(plane.get('routes') or [])} alts={len(plane.get('alts') or [])}", file=sys.stderr, flush=True)
    routes = audit_routes(
        plane, dlmm.get_multiple, dlmm.rpc, dlmm, args.limit or None, args.pool or None,
    )
    ours = _program_info(dlmm.get_multiple, OUR_EXEC, DISC)
    hops = _hops_info(dlmm.get_multiple)
    ready_n = sum(1 for r in routes if r.get("RACE_READY"))
    blockers = []
    if args.auth_cap is not None:
        live_ready = Path(args.auth_cap) / "READY"
        live_cap = str(Path(args.auth_cap))
    else:
        sys.path.insert(0, str(Path("/home/louis/arb-exec/scripts")))
        sys.path.insert(0, str(here.parents[1] / "arb-exec" / "scripts"))
        import exec_gates as gates  # noqa: E402
        live_ready = gates.current_auth_ready()
        live_cap, _ = gates.current_auth_cap()
    arm_ready = Path("/home/louis/captures/state008/READY")
    if not live_ready.exists():
        blockers.append(
            f"Live AUTH READY absent ({live_ready}). Generation cap={live_cap}. "
            "This is the current soak, not the oneshot arm path."
        )
    if not arm_ready.exists():
        blockers.append(
            f"Oneshot arm READY absent ({arm_ready}). "
            "oneshot_live still refuses until this exact path exists."
        )
    if ready_n == 0:
        blockers.append("No route passed template, alt, current-bank vector, ATA, and unsigned sim together.")
    if hops.get("so_bytes") and hops.get("programdata_len") and hops["programdata_len"] - 45 != hops["so_bytes"]:
        blockers.append("ARBHOPS0 programdata ELF length does not match the retained hops.so.")
    blockers.append("paper_orbit was not restarted by this audit. A live hour still has to be searched after STATE is ready.")
    doc = {
        "routes": routes,
        "our_exec": ours,
        "hops": hops,
        "wire_hurdle": 400_000 * 800_000 // 1_000_000 + 5_000 + 150_000 + 50_000,
        "blockers": blockers,
    }
    payload = json.dumps(doc)
    if args.out is None or str(args.out) == "-":
        sys.stdout.write(payload)
        sys.stdout.write("\n")
        return 0
    args.out.write_text(payload + "\n", encoding="utf-8")
    report = args.out.parent / "EXEC_READY_REPORT.md"
    report.write_text(render_report(doc), encoding="utf-8")
    print(f"wrote {args.out}", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
