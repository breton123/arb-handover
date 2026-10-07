#!/usr/bin/env python3
"""Per-hop semantic account vectors for ARBHOPS0.

Derive every PDA independently. Flatten to instruction order second.
Never reuse a flattened extra from another hop because it "looks global."
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
import sys

_CAP = Path("/home/louis/arb-cap")
if not _CAP.is_dir():
    _CAP = Path(__file__).resolve().parents[2] / "arb-cap"
sys.path.insert(0, str(_CAP))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import exec_live002b as x  # noqa: E402
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

DLMM = x.DLMM
PUMP = x.PUMP
SOL = x.SOL
TOKENKEG = x.TOKENKEG
TOKEN2022 = x.TOKEN2022
SYSTEM = x.SYSTEM
ATA_PROG = x.ATA_PROG
FEE_PROG = x.FEE_PROG

SEQ_SHORT = {
    "dlmm-dlmm": "dd",
    "dlmm-pump": "dp",
    "pump-dlmm": "pd",
    "pump-pump": "pp",
    "dlmm-dlmm-dlmm": "ddd",
    "dlmm-dlmm-pump": "ddp",
    "pump-dlmm-dlmm": "pdd",
}

PUMP_HEAD_ROLES = (
    "pool", "user", "global_config", "base_mint", "quote_mint",
    "user_base_ata", "user_quote_ata", "pool_base_vault", "pool_quote_vault",
    "protocol_fee_recipient", "protocol_fee_recipient_quote_ata",
    "token_program_base", "token_program_quote",
    "system_program", "associated_token_program",
    "event_authority", "program",
    "creator_vault_ata", "creator_vault",
)
PUMP_BUY_TAIL = (
    "global_volume_accumulator", "user_volume_accumulator",
    "fee_config", "fee_program", "pool_v2",
    "fee_recipient", "fee_recipient_quote_ata",
)
PUMP_SELL_TAIL = (
    "fee_config", "fee_program", "pool_v2",
    "fee_recipient", "fee_recipient_quote_ata",
)
DLMM_ROLES = (
    "lb_pair", "bitmap", "vault_x", "vault_y", "oracle",
    "host", "mint_x", "mint_y", "bin0", "bin1",
)

# Hop-local PDAs / ATAs. Globals are still derived per hop, never copied.
PUMP_PDA_ROLES = {
    "global_config", "event_authority", "creator_vault", "creator_vault_ata",
    "global_volume_accumulator", "user_volume_accumulator", "pool_v2",
    "protocol_fee_recipient_quote_ata", "fee_recipient_quote_ata",
}
DLMM_PDA_ROLES = {"oracle", "bin0", "bin1"}

ANCHOR_ROLE = {
    "pool": "pool",
    "user": "user",
    "global_config": "global_config",
    "global": "global_config",
    "base_mint": "base_mint",
    "quote_mint": "quote_mint",
    "user_base_token_account": "user_base_ata",
    "user_quote_token_account": "user_quote_ata",
    "pool_base_token_account": "pool_base_vault",
    "pool_quote_token_account": "pool_quote_vault",
    "protocol_fee_recipient": "protocol_fee_recipient",
    "protocol_fee_recipient_token_account": "protocol_fee_recipient_quote_ata",
    "coin_creator_vault_authority": "creator_vault",
    "coin_creator_vault_ata": "creator_vault_ata",
    "coin_creator_vault": "creator_vault",
    "creator_vault": "creator_vault",
    "global_volume_accumulator": "global_volume_accumulator",
    "user_volume_accumulator": "user_volume_accumulator",
    "fee_config": "fee_config",
    "coin_creator": "coin_creator",
    "pool_v2": "pool_v2",
    "fee_recipient": "fee_recipient",
    "protocol_fee_recipient_quote_ata": "protocol_fee_recipient_quote_ata",
}

_MINT_TOK: dict[str, str] = {SOL: TOKENKEG}


def mint_tok(mint: str) -> str:
    if mint in _MINT_TOK:
        return _MINT_TOK[mint]
    info = d.get_multiple([mint])[0]
    if not info:
        raise RuntimeError(f"mint missing {mint[:8]}")
    owner = info.get("owner")
    if owner not in (TOKENKEG, TOKEN2022):
        raise RuntimeError(f"mint owner {owner}")
    _MINT_TOK[mint] = owner
    return owner


def pda_ex(seeds: list[bytes], program: str) -> tuple[str, int]:
    digest = d.find_pda(seeds, d.b58decode(program))
    pk = d._pk(digest)
    prog = d.b58decode(program)
    for bump in range(255, -1, -1):
        h = hashlib.sha256()
        for s in seeds:
            h.update(s)
        h.update(bytes([bump]))
        h.update(prog)
        h.update(b"ProgramDerivedAddress")
        if h.digest() == digest:
            return pk, bump
    return pk, -1


def pump_roles(buy: bool) -> tuple[str, ...]:
    return PUMP_HEAD_ROLES + (PUMP_BUY_TAIL if buy else PUMP_SELL_TAIL)


def parse_pump_fields(raw: bytes) -> dict:
    p = live.parse_pump_pool(raw)
    if not p:
        raise RuntimeError("pump parse")
    pool_creator = d._pk(raw[11:43]) if len(raw) >= 43 else None
    blob = raw[211:243] if len(raw) >= 243 else bytes(32)
    # Pump seeds [b"creator_vault", coin_creator] even when coin_creator is default.
    # Do not substitute pool_creator — that is a different pubkey and fails 2006.
    coin = d._pk(blob)
    return {
        "base": d._pk(p["base"]),
        "quote": d._pk(p["quote"]),
        "vault_base": d._pk(p["vault_base"]),
        "vault_quote": d._pk(p["vault_quote"]),
        "coin_creator": coin,
        "pool_creator": pool_creator,
        "coin_creator_source": "coin_creator" if blob != bytes(32) else "default",
    }


def pick_fee_recipient(quote: str) -> tuple[str, str]:
    tok = TOKENKEG if quote == SOL else mint_tok(quote)
    for rec in x.FEE_RECIPIENTS:
        ata = x.ata(rec, quote, tok)
        if x.exists(ata):
            return rec, ata
    rec = x.FEE_RECIPIENT
    return rec, x.ata(rec, quote, tok)


@dataclass
class PumpHopAccounts:
    pool: str
    global_config: str
    base_mint: str
    quote_mint: str
    user_base_ata: str
    user_quote_ata: str
    pool_base_vault: str
    pool_quote_vault: str
    protocol_fee_recipient: str
    protocol_fee_recipient_quote_ata: str
    coin_creator: str
    creator_vault: str
    creator_vault_ata: str
    global_volume_accumulator: str
    user_volume_accumulator: str
    pool_v2: str
    token_program_base: str
    token_program_quote: str
    associated_token_program: str
    system_program: str
    event_authority: str
    program: str
    fee_config: str
    fee_program: str
    fee_recipient: str
    fee_recipient_quote_ata: str
    buy: bool
    bumps: dict = field(default_factory=dict)
    seeds: dict = field(default_factory=dict)

    def flatten(self, wallet: str) -> list[str]:
        head = [
            self.pool, wallet, self.global_config, self.base_mint, self.quote_mint,
            self.user_base_ata, self.user_quote_ata,
            self.pool_base_vault, self.pool_quote_vault,
            self.protocol_fee_recipient, self.protocol_fee_recipient_quote_ata,
            self.token_program_base, self.token_program_quote,
            self.system_program, self.associated_token_program,
            self.event_authority, self.program,
            self.creator_vault_ata, self.creator_vault,
        ]
        if self.buy:
            return head + [
                self.global_volume_accumulator, self.user_volume_accumulator,
                self.fee_config, self.fee_program, self.pool_v2,
                self.fee_recipient, self.fee_recipient_quote_ata,
            ]
        return head + [
            self.fee_config, self.fee_program, self.pool_v2,
            self.fee_recipient, self.fee_recipient_quote_ata,
        ]

    def role_map(self, wallet: str) -> dict[str, str]:
        return dict(zip(pump_roles(self.buy), self.flatten(wallet)))


@dataclass
class DlmmHopAccounts:
    pool: str
    bitmap: str
    vault_x: str
    vault_y: str
    oracle: str
    host: str
    mint_x: str
    mint_y: str
    bin0: str
    bin1: str
    bin0_index: int
    bin1_index: int
    active_id: int
    bumps: dict = field(default_factory=dict)
    seeds: dict = field(default_factory=dict)

    def flatten(self) -> list[str]:
        return [
            self.pool, self.bitmap, self.vault_x, self.vault_y, self.oracle,
            self.host, self.mint_x, self.mint_y, self.bin0, self.bin1,
        ]

    def role_map(self) -> dict[str, str]:
        return dict(zip(DLMM_ROLES, self.flatten()))


def derive_pump_hop(
    pair: str,
    buy: bool,
    wallet: str,
    user_base: str,
    user_quote: str,
    meta: dict | None = None,
) -> PumpHopAccounts:
    """Build one Pump hop from pool bytes. No dump-cache, no sibling reuse."""
    if meta is None:
        pacc = d.get_multiple([pair])[0]
        if not pacc:
            raise RuntimeError(f"VECTOR_FAIL role=pool reason=MISSING pool={pair}")
        meta = parse_pump_fields(pacc["data"])
    mx = meta["base"]
    my = meta["quote"]
    coin = meta.get("coin_creator")
    if not coin:
        raise RuntimeError(
            f"VECTOR_FAIL role=coin_creator reason=MISSING pool={pair} "
            f"pool_creator={meta.get('pool_creator')}"
        )
    tok_b = mint_tok(mx)
    tok_q = mint_tok(my)
    creator_vault, cv_bump = pda_ex([b"creator_vault", d.b58decode(coin)], PUMP)
    creator_ata = x.ata(creator_vault, my, tok_q)
    global_pk, g_bump = pda_ex([b"global_config"], PUMP)
    event, e_bump = pda_ex([b"__event_authority"], PUMP)
    gvol, gv_bump = pda_ex([b"global_volume_accumulator"], PUMP)
    uvol, uv_bump = pda_ex(
        [b"user_volume_accumulator", d.b58decode(wallet)], PUMP,
    )
    pool_v2, v2_bump = pda_ex([b"pool-v2", d.b58decode(mx)], PUMP)
    fb = x.pump_fallback()
    proto = fb["proto_fee"]
    proto_ata = x.ata(proto, my, tok_q)
    fee_rec, fee_ata = pick_fee_recipient(my)
    return PumpHopAccounts(
        pool=pair,
        global_config=global_pk,
        base_mint=mx,
        quote_mint=my,
        user_base_ata=user_base,
        user_quote_ata=user_quote,
        pool_base_vault=meta["vault_base"],
        pool_quote_vault=meta["vault_quote"],
        protocol_fee_recipient=proto,
        protocol_fee_recipient_quote_ata=proto_ata,
        coin_creator=coin,
        creator_vault=creator_vault,
        creator_vault_ata=creator_ata,
        global_volume_accumulator=gvol,
        user_volume_accumulator=uvol,
        pool_v2=pool_v2,
        token_program_base=tok_b,
        token_program_quote=tok_q,
        associated_token_program=ATA_PROG,
        system_program=SYSTEM,
        event_authority=event,
        program=PUMP,
        fee_config=x.FEE_CFG_LIVE,
        fee_program=FEE_PROG,
        fee_recipient=fee_rec,
        fee_recipient_quote_ata=fee_ata,
        buy=buy,
        bumps={
            "global_config": g_bump,
            "event_authority": e_bump,
            "creator_vault": cv_bump,
            "global_volume_accumulator": gv_bump,
            "user_volume_accumulator": uv_bump,
            "pool_v2": v2_bump,
        },
        seeds={
            "global_config": ["global_config"],
            "event_authority": ["__event_authority"],
            "creator_vault": ["creator_vault", coin],
            "global_volume_accumulator": ["global_volume_accumulator"],
            "user_volume_accumulator": ["user_volume_accumulator", wallet],
            "pool_v2": ["pool-v2", mx],
            "creator_vault_ata": ["ata", creator_vault, my, tok_q],
            "protocol_fee_recipient_quote_ata": ["ata", proto, my, tok_q],
            "fee_recipient_quote_ata": ["ata", fee_rec, my, tok_q],
        },
    )


def derive_dlmm_hop(pair: str, meta: dict | None = None) -> DlmmHopAccounts:
    """Bin arrays are PDAs of (pool, bin_array_index), not generic pubkeys."""
    if meta is None:
        sn = d.snapshot_pool(pair, [pair], None)
        if not sn or not sn.get("lb"):
            raise RuntimeError(f"VECTOR_FAIL role=lb_pair reason=SNAPSHOT pool={pair}")
        lb = sn["lb"]
        meta = {
            "mint_x": d._pk(lb["token_x"]),
            "mint_y": d._pk(lb["token_y"]),
            "vault_x": d._pk(lb["vault_x"]),
            "vault_y": d._pk(lb["vault_y"]),
            "active_id": lb["active_id"],
        }
    active = int(meta["active_id"])
    base_idx = d.bin_array_index(active)
    bins: list[tuple[int, str, int]] = []
    for i in (base_idx, base_idx + 1, base_idx - 1, base_idx + 2, base_idx - 2):
        pk, bump = pda_ex(
            [b"bin_array", d.b58decode(pair), struct.pack("<q", i)],
            DLMM,
        )
        if pk not in [b[1] for b in bins] and x.exists(pk):
            bins.append((i, pk, bump))
        if len(bins) == 2:
            break
    if not bins:
        raise RuntimeError(f"VECTOR_FAIL role=bin0 reason=MISSING pool={pair} active_id={active}")
    if len(bins) == 1:
        bins.append(bins[0])
    oracle, o_bump = pda_ex([b"oracle", d.b58decode(pair)], DLMM)
    return DlmmHopAccounts(
        pool=pair,
        bitmap=DLMM,
        vault_x=meta["vault_x"],
        vault_y=meta["vault_y"],
        oracle=oracle,
        host=DLMM,
        mint_x=meta["mint_x"],
        mint_y=meta["mint_y"],
        bin0=bins[0][1],
        bin1=bins[1][1],
        bin0_index=bins[0][0],
        bin1_index=bins[1][0],
        active_id=active,
        bumps={"oracle": o_bump, "bin0": bins[0][2], "bin1": bins[1][2]},
        seeds={
            "oracle": ["oracle", pair],
            "bin0": ["bin_array", pair, bins[0][0]],
            "bin1": ["bin_array", pair, bins[1][0]],
        },
    )


def vector_fail(route, hop, role, **kw) -> dict:
    rec = {"route": route, "hop": hop, "role": role}
    rec.update(kw)
    return rec


def print_vector_fail_block(f: dict) -> None:
    print("VECTOR_FAIL", flush=True)
    order = (
        "seq", "route", "hop", "role", "reason", "actual", "expected",
        "caused_by", "seed_creator", "seed_mint", "seed_wallet", "seed_pool",
        "seed_bin_index", "bump", "pool", "mint", "coin_creator",
        "fee_recipient", "creator_vault", "volume_accumulator",
        "user_volume_accumulator", "pool_v2", "direction",
    )
    seen = set()
    for k in order:
        if k in f and f[k] is not None:
            print(f"{k}={f[k]}", flush=True)
            seen.add(k)
    for k, v in f.items():
        if k not in seen and k not in ("route", "hop", "role") and v is not None:
            print(f"{k}={v}", flush=True)


def print_vector_fails(fails: list[dict]) -> None:
    for f in fails:
        bits = [f"VECTOR_FAIL route={f.get('route')} hop={f.get('hop')} role={f.get('role')}"]
        for k, v in f.items():
            if k in ("route", "hop", "role") or v is None:
                continue
            bits.append(f"{k}={v}")
        print("  " + " ".join(bits), flush=True)
        if f.get("reason") in ("PDA_SEEDS", "BIN_ARRAY", "ANCHOR_2006", "ANCHOR_3005"):
            print_vector_fail_block(f)


def validate_pump_keys(
    hop_keys: list[str],
    expected: PumpHopAccounts,
    wallet: str,
    rid,
    hi: int,
    seq: str,
) -> list[dict]:
    want = expected.flatten(wallet)
    roles = pump_roles(expected.buy)
    fails = []
    for role, got, exp in zip(roles, hop_keys, want):
        if got == exp:
            continue
        reason = "PDA_SEEDS" if role in PUMP_PDA_ROLES else "MISMATCH"
        fails.append(vector_fail(
            rid, hi, role,
            seq=seq,
            reason=reason,
            actual=got,
            expected=exp,
            direction="buy" if expected.buy else "sell",
            pool=expected.pool,
            mint=expected.base_mint,
            coin_creator=expected.coin_creator,
            fee_recipient=expected.fee_recipient,
            creator_vault=expected.creator_vault,
            volume_accumulator=expected.global_volume_accumulator,
            user_volume_accumulator=expected.user_volume_accumulator,
            pool_v2=expected.pool_v2,
            bump=expected.bumps.get(role),
            seed_creator=expected.coin_creator if role in (
                "creator_vault", "creator_vault_ata",
            ) else None,
            seed_mint=expected.base_mint if role == "pool_v2" else (
                expected.quote_mint if role.endswith("_ata") else None
            ),
            seed_wallet=wallet if role == "user_volume_accumulator" else None,
        ))
    return fails


def validate_dlmm_keys(
    hop_keys: list[str],
    expected: DlmmHopAccounts,
    rid,
    hi: int,
    seq: str,
) -> list[dict]:
    want = expected.flatten()
    fails = []
    for role, got, exp in zip(DLMM_ROLES, hop_keys, want):
        if got == exp:
            continue
        reason = "BIN_ARRAY" if role in ("bin0", "bin1") else (
            "PDA_SEEDS" if role in DLMM_PDA_ROLES else "MISMATCH"
        )
        fails.append(vector_fail(
            rid, hi, role,
            seq=seq,
            reason=reason,
            actual=got,
            expected=exp,
            pool=expected.pool,
            seed_pool=expected.pool,
            seed_bin_index=expected.bin0_index if role == "bin0" else (
                expected.bin1_index if role == "bin1" else None
            ),
            bump=expected.bumps.get(role),
        ))
    return fails


def parse_anchor_constraint(logs: list[str]) -> dict:
    caused = None
    left = None
    right = None
    number = None
    take_left = False
    take_right = False
    hop_venue_invokes = []
    for line in logs or []:
        s = str(line)
        if " invoke [" in s:
            if PUMP[:8] in s or "pAMM" in s:
                hop_venue_invokes.append("pump")
            elif DLMM[:8] in s or "LBUZ" in s:
                hop_venue_invokes.append("dlmm")
        m = re.search(r"caused by account:\s*([A-Za-z0-9_]+)", s)
        if m:
            caused = m.group(1)
        m = re.search(r"Error Number:\s*(\d+)", s)
        if m:
            number = int(m.group(1))
        if s.rstrip().endswith("Left:"):
            take_left = True
            continue
        if s.rstrip().endswith("Right:"):
            take_right = True
            continue
        if take_left:
            left = s.split()[-1] if s.split() else s
            take_left = False
        if take_right:
            right = s.split()[-1] if s.split() else s
            take_right = False
        if "0x7d6" in s or "custom program error: 2006" in s.lower():
            number = number or 2006
        if "0xbbd" in s or "custom program error: 3005" in s.lower():
            number = number or 3005
    return {
        "caused_by": caused,
        "actual": left,
        "expected": right,
        "error_number": number,
        "invokes": hop_venue_invokes,
    }


def explain_sim_fail(
    seq: str,
    rid,
    hops_sem: list,
    wallet: str,
    sim: dict,
) -> list[dict]:
    """Turn Pump 2006 / DLMM 3005 logs into VECTOR_FAIL PDA_SEEDS rows."""
    logs = sim.get("logs") or []
    parsed = parse_anchor_constraint(logs)
    num = parsed.get("error_number")
    if num not in (2006, 3005) and not parsed.get("caused_by"):
        return []
    if num is None and parsed.get("caused_by"):
        num = 2006
    reason = "ANCHOR_2006" if num == 2006 else (
        "ANCHOR_3005" if num == 3005 else "ANCHOR"
    )
    caused = parsed.get("caused_by")
    role = ANCHOR_ROLE.get(caused or "", caused or "unknown")
    hop_i = None
    venues = parsed.get("invokes") or []
    if venues:
        hop_i = min(len(venues), len(hops_sem)) - 1
    if hop_i is None:
        for i, h in enumerate(hops_sem):
            if num == 2006 and isinstance(h, PumpHopAccounts):
                hop_i = i
            if num == 3005 and isinstance(h, DlmmHopAccounts):
                hop_i = i
        hop_i = 0 if hop_i is None else hop_i
    hop = hops_sem[hop_i] if hop_i < len(hops_sem) else None
    rec = vector_fail(
        rid, hop_i, role,
        seq=seq,
        reason=reason,
        caused_by=caused,
        actual=parsed.get("actual"),
        expected=parsed.get("expected"),
        direction=("buy" if isinstance(hop, PumpHopAccounts) and hop.buy
                   else "sell" if isinstance(hop, PumpHopAccounts)
                   else "dlmm"),
    )
    if isinstance(hop, PumpHopAccounts):
        rec.update({
            "pool": hop.pool,
            "mint": hop.base_mint,
            "coin_creator": hop.coin_creator,
            "fee_recipient": hop.fee_recipient,
            "creator_vault": hop.creator_vault,
            "volume_accumulator": hop.global_volume_accumulator,
            "user_volume_accumulator": hop.user_volume_accumulator,
            "pool_v2": hop.pool_v2,
            "bump": hop.bumps.get(role),
            "seed_creator": hop.coin_creator,
            "seed_mint": hop.base_mint,
        })
        got = hop.role_map(wallet).get(role)
        exp_pda = None
        if role == "creator_vault":
            exp_pda = hop.creator_vault
        elif role == "pool_v2":
            exp_pda = hop.pool_v2
        elif role == "creator_vault_ata":
            exp_pda = hop.creator_vault_ata
        if rec.get("actual") is None:
            rec["actual"] = got
        if rec.get("expected") is None:
            rec["expected"] = exp_pda
    if isinstance(hop, DlmmHopAccounts):
        rec.update({
            "pool": hop.pool,
            "seed_pool": hop.pool,
            "seed_bin_index": hop.bin0_index if role == "bin0" else hop.bin1_index,
            "bump": hop.bumps.get(role),
        })
    return [rec]


def hops_simulate(tx: bytes) -> dict:
    """Full-log simulate. 2006 lives in logs; hops maps CPI fail to Custom(5)."""
    import base64
    raw = base64.b64encode(tx).decode()
    try:
        res = d.rpc("simulateTransaction", [
            raw,
            {
                "encoding": "base64",
                "replaceRecentBlockhash": True,
                "sigVerify": False,
                "innerInstructions": True,
            },
        ])
    except RuntimeError as e:
        return {"err": "rpc", "rpc": str(e), "cu": None, "logs": [], "accepted": False}
    val = res.get("value") or res
    logs = val.get("logs") or []
    err = val.get("err")
    cu = val.get("unitsConsumed")
    print(f"  simulate err={err} cu={cu} logs={len(logs)}", flush=True)
    for line in logs[:20]:
        print("   ", str(line)[:180], flush=True)
    if len(logs) > 20:
        print("    ...", flush=True)
        for line in logs[-6:]:
            print("   ", str(line)[:180], flush=True)
    return {
        "err": err,
        "cu": cu,
        "logs": logs,
        "inner": len(val.get("innerInstructions") or []),
        "accepted": True,
    }


def golden_dirs() -> list[Path]:
    here = Path(__file__).resolve().parents[1] / "tests" / "golden_hops"
    fam = Path("/home/louis/arb-cap/fam6/golden_hops")
    out = [here]
    if Path("/home/louis/arb-cap/fam6").is_dir():
        out.append(fam)
    for p in out:
        p.mkdir(parents=True, exist_ok=True)
    return out


def golden_path(seq: str, root: Path | None = None) -> Path:
    short = SEQ_SHORT[seq]
    base = root or golden_dirs()[0]
    return base / f"{short}.json"


def pump_writable(buy: bool, i: int) -> bool:
    if buy:
        return i in (0, 1, 5, 6, 7, 8, 10, 17, 19, 20, 25)
    return i in (0, 1, 5, 6, 7, 8, 10, 17, 23)


def dlmm_writable(i: int) -> bool:
    return i in (0, 2, 3, 4, 8, 9)


def static_set(wallet: str, hops_pid: str | None) -> set[str]:
    s = {wallet, TOKENKEG, TOKEN2022, SYSTEM, ATA_PROG, x.MEMO, DLMM, PUMP, FEE_PROG}
    if hops_pid:
        s.add(hops_pid)
    return s


def account_row(role: str, pk: str, writable: bool, signer: bool,
                statics: set[str], token_program: str | None = None) -> dict:
    return {
        "role": role,
        "pubkey": pk,
        "writable": writable,
        "signer": signer,
        "token_program": token_program,
        "alt_or_static": "static" if pk in statics else "alt",
    }


def build_golden_doc(
    seq: str,
    r: dict,
    wallet: str,
    keys: list[str],
    hops_sem: list,
    sim: dict,
    used_alt: bool,
    hops_pid: str | None,
) -> dict:
    statics = static_set(wallet, hops_pid)
    shared_roles = (
        "authority", "user0", "user1", "user2",
        "tokenkeg", "token2022", "memo", "dlmm_prog", "dlmm_event",
    )
    shared = []
    for i, role in enumerate(shared_roles):
        pk = keys[i]
        tok = None
        if role.startswith("user"):
            tok = TOKENKEG if i == 1 else None
        shared.append(account_row(
            role, pk,
            writable=i in (0, 1, 2, 3),
            signer=i == 0,
            statics=statics,
            token_program=tok,
        ))
    hop_docs = []
    for h in hops_sem:
        if isinstance(h, PumpHopAccounts):
            roles = pump_roles(h.buy)
            accs = h.flatten(wallet)
            rows = []
            for i, (role, pk) in enumerate(zip(roles, accs)):
                tok = None
                if role in ("user_base_ata", "pool_base_vault"):
                    tok = h.token_program_base
                elif role in ("user_quote_ata", "pool_quote_vault",
                              "protocol_fee_recipient_quote_ata",
                              "creator_vault_ata", "fee_recipient_quote_ata"):
                    tok = h.token_program_quote
                elif role == "token_program_base":
                    tok = h.token_program_base
                elif role == "token_program_quote":
                    tok = h.token_program_quote
                rows.append(account_row(
                    role, pk, pump_writable(h.buy, i),
                    signer=pk == wallet and role == "user",
                    statics=statics, token_program=tok,
                ))
            hop_docs.append({
                "venue": "pump",
                "direction": "buy" if h.buy else "sell",
                "pool": h.pool,
                "coin_creator": h.coin_creator,
                "bumps": h.bumps,
                "seeds": h.seeds,
                "accounts": rows,
            })
        else:
            accs = h.flatten()
            rows = []
            for i, (role, pk) in enumerate(zip(DLMM_ROLES, accs)):
                tok = None
                if role == "vault_x":
                    tok = mint_tok(h.mint_x)
                elif role == "vault_y":
                    tok = mint_tok(h.mint_y)
                rows.append(account_row(
                    role, pk, dlmm_writable(i),
                    signer=False, statics=statics, token_program=tok,
                ))
            hop_docs.append({
                "venue": "dlmm",
                "direction": "swap2",
                "pool": h.pool,
                "active_id": h.active_id,
                "bin0_index": h.bin0_index,
                "bin1_index": h.bin1_index,
                "bumps": h.bumps,
                "seeds": h.seeds,
                "accounts": rows,
            })
    rebuilt = [a["pubkey"] for a in shared]
    for hd in hop_docs:
        rebuilt.extend(a["pubkey"] for a in hd["accounts"])
    return {
        "seq": seq,
        "id": r.get("id"),
        "custom6": True,
        "cu": sim.get("cu"),
        "alt": used_alt,
        "n_acc": len(keys),
        "keys": keys,
        "shared": shared,
        "hops": hop_docs,
        "semantic_match": rebuilt == keys,
    }


def persist_golden(
    seq: str,
    r: dict,
    wallet: str,
    keys: list[str],
    hops_sem: list,
    sim: dict,
    used_alt: bool,
    hops_pid: str | None,
) -> Path | None:
    short = SEQ_SHORT.get(seq)
    if not short:
        return None
    doc = build_golden_doc(seq, r, wallet, keys, hops_sem, sim, used_alt, hops_pid)
    written = None
    for root in golden_dirs():
        path = root / f"{short}.json"
        if path.exists():
            old = json.loads(path.read_text(encoding="utf-8"))
            if old.get("custom6"):
                print(f"GOLDEN  frozen {short} id={old.get('id')} (keep)", flush=True)
                written = written or path
                continue
        path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(
            f"GOLDEN  wrote {path} seq={seq} id={r.get('id')} "
            f"n_acc={len(keys)} match={doc['semantic_match']}",
            flush=True,
        )
        written = path
    return written


def check_golden(seq: str, rid, keys: list[str]) -> list[dict]:
    path = golden_path(seq)
    if not path.exists():
        return []
    gold = json.loads(path.read_text(encoding="utf-8"))
    if gold.get("id") != rid:
        return []
    if gold.get("keys") != keys:
        return [vector_fail(
            rid, 0, "golden",
            seq=seq,
            reason="GOLDEN_REGRESS",
            actual=f"n={len(keys)}",
            expected=f"n={len(gold.get('keys') or [])} id={gold.get('id')}",
        )]
    return []


def flatten_fixture(doc: dict) -> list[str]:
    keys = [a["pubkey"] for a in doc.get("shared") or []]
    for h in doc.get("hops") or []:
        keys.extend(a["pubkey"] for a in h.get("accounts") or [])
    return keys
