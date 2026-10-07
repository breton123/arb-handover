#!/usr/bin/env python3
"""EXEC-LIVE-002B — v0 + one ALT. EXEC-002 legacy template stays frozen.

35 logical OUR_EXEC accounts stay in EXEC-001 order. ALT changes encoding only.
Optional-none bitmap/host = DLMM program id. No dummy PDAs.
Never prints secrets. Never sends the arb.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-cap"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

from solders.compute_budget import set_compute_unit_price
from solders.hash import Hash
from solders.instruction import AccountMeta, Instruction
from solders.keypair import Keypair
from solders.message import Message
from solders.pubkey import Pubkey
from solders.system_program import TransferParams, transfer
from solders.transaction import Transaction

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / ".deploy"
OUT = ROOT.parent / "arb-cap" / "exec_live002b"
PLANE = DEPLOY / "alt_plane.json"


class AltPlaneMiss(RuntimeError):
    pass

DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
PAIR_DLMM = "DAErPzgiYQDhDwpMRgdvikXaZmnmznc3du3jy9i1AABk"
PAIR_PUMP = "BRjMA8UALNp3diTKHfdeAn5riQrYPoaYez7KhYWD4oEs"
SOL = "So11111111111111111111111111111111111111112"
TOKENKEG = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKEN2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
SYSTEM = "11111111111111111111111111111111"
ATA_PROG = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
MEMO = "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr"
FEE_PROG = "pfeeUxB6jkeY1Hxd7CsFCAjcbHA9rWtchMGdZ6VojVZ"
CU_PROG = "ComputeBudget111111111111111111111111111111"
ALT_PROG = "AddressLookupTab1e1111111111111111111111111"
FEE_CFG_LIVE = "5PHirr8joyTMp9JMm6nW7hNDVyEYdkzDqazxPD7RaTjx"
# Official 8 Pump AMM buyback fee recipients (BREAKING_FEE_RECIPIENT.md).
# Any one is valid; does not vary by pool. Quote ATA = ATA(recipient, WSOL, TOKENKEG).
FEE_RECIPIENTS = [
    "5YxQFdt3Tr9zJLvkFccqXVUwhdTWJQc1fFg2YPbxvxeD",
    "9M4giFFMxmFGXtc3feFzRai56WbBqehoSeRE5GK7gf7",
    "GXPFM2caqTtQYC2cJ5yJRi9VDkpsYZXzYdwYpGnLmtDL",
    "3BpXnfJaUTiwXnJNe7Ej1rcbzqTTQUvLShZaWazebsVR",
    "5cjcW9wExnJJiqgLjq7DEG75Pm6JBgE1hNv4B2vHXUW6",
    "EHAAiTxcdDwQ3U4bU6YcMsQGaekdzLS3B5SmYo46kJtL",
    "5eHhjP8JaYkz83CWwvGU2uMUXefd3AazWGx4gpcuEEYD",
    "A7hAgCzFw14fejgCp387JUJRMNyz4j89JKnhtKU8piqW",
]
FEE_RECIPIENT = "5cjcW9wExnJJiqgLjq7DEG75Pm6JBgE1hNv4B2vHXUW6"

V0_TX_LEN = 623
V0_OFF_SIG = 1
V0_OFF_MSG = 65
V0_MSG_LEN = 558
V0_OFF_BH = 422
V0_OFF_PRICE = 467
V0_OFF_DIR = 546
V0_OFF_AMT = 547
V0_OFF_MIN = 555
CU_LIMIT = 400000
TINY_IN = 10_000
MIN_GUARD = 1_000_000_000
DISC = b"ARBEXEC0"

NAMES = [
    "authority", "user_quote", "user_base", "token_program", "system_program",
    "ata_program", "memo_program", "dlmm_program", "dlmm_event_auth",
    "dlmm_lb_pair", "dlmm_bitmap", "dlmm_reserve_x", "dlmm_reserve_y",
    "dlmm_oracle", "dlmm_host_fee", "dlmm_token_x_mint", "dlmm_token_y_mint",
    "dlmm_bin_0", "dlmm_bin_1", "pump_program", "pump_event_auth", "pump_pool",
    "pump_global", "pump_fee_config", "pump_fee_program", "pump_base_mint",
    "pump_quote_mint", "pump_pool_base", "pump_pool_quote", "pump_proto_fee",
    "pump_proto_fee_ata", "pump_creator_ata", "pump_creator_auth",
    "pump_global_vol", "pump_user_vol",
]

IX_IDX = bytes([
    0, 11, 12, 3, 4, 5, 6, 7, 26, 13,
    7, 14, 15, 16, 7, 27, 28, 17, 18, 8,
    29, 19, 30, 31, 10, 27, 28, 20, 21, 32,
    22, 23, 33, 34, 24,
    9,
    19, 0, 30, 27, 28, 12, 11, 20, 21, 32,
    22, 9, 3, 4, 5, 29, 8, 23, 33, 31,
    10, 34, 35, 25,
])
ALT_WR_IX = bytes(range(14)) + bytes([23])
ALT_RO_IX = bytes(range(14, 23)) + bytes([24])
# BuyExactQuoteIn: remaining is sell 24 with gvol+uvol inserted after creator_auth.
IX_IDX_BUY = bytes([
    0, 11, 12, 3, 4, 5, 6, 7, 27, 13,
    7, 14, 15, 16, 7, 28, 29, 17, 18, 8,
    30, 19, 31, 32, 10, 28, 29, 20, 21, 33,
    22, 23, 34, 35, 24,
    9,
    19, 0, 31, 28, 29, 12, 11, 20, 21, 33,
    22, 9, 3, 4, 5, 30, 8, 23, 34, 26, 24, 32,
    10, 35, 36, 25,
])
ALT_WR_IX_BUY = bytes(range(14)) + bytes([23, 25])
ALT_RO_IX_BUY = bytes(range(14, 23)) + bytes([24])
V0_TX_LEN_BUY = 626
V0_MSG_LEN_BUY = 561
V0_OFF_DIR_BUY = 548
V0_OFF_AMT_BUY = 549
V0_OFF_MIN_BUY = 557


def b58_any(s: str) -> bytes:
    alph = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n = 0
    for c in s:
        n = n * 58 + alph.index(c)
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    h = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\x00" * pad + h


def payer_kp() -> Keypair:
    secret = b58_any(os.environ["PRIVATE_KEY"].strip())
    if len(secret) == 64:
        return Keypair.from_bytes(secret)
    if len(secret) == 32:
        return Keypair.from_seed(secret)
    raise SystemExit(f"PRIVATE_KEY length {len(secret)}")


def ata(owner: str, mint: str, tok: str) -> str:
    return d._pk(d.find_pda(
        [d.b58decode(owner), d.b58decode(tok), d.b58decode(mint)],
        d.b58decode(ATA_PROG),
    ))


def program_v3() -> str:
    path = DEPLOY / "program-v3.json"
    if not path.exists():
        raise SystemExit("missing .deploy/program-v3.json")
    raw = json.loads(path.read_text(encoding="utf-8"))
    return d._pk(bytes(raw[32:64]))


def exists(pk: str) -> bool:
    return d.get_multiple([pk])[0] is not None


def latest_blockhash() -> Hash:
    bh = d.rpc("getLatestBlockhash", [{"commitment": "confirmed"}])
    return Hash.from_string(bh["value"]["blockhash"])


def send_ok(payer: Keypair, ixs: list[Instruction]) -> str:
    last = None
    for _ in range(6):
        try:
            msg = Message.new_with_blockhash(ixs, payer.pubkey(), latest_blockhash())
            tx = Transaction.new_unsigned(msg)
            tx.sign([payer], msg.recent_blockhash)
            sig = d.rpc("sendTransaction", [
                base64.b64encode(bytes(tx)).decode(),
                {"encoding": "base64", "skipPreflight": False, "preflightCommitment": "confirmed"},
            ])
            t0 = time.time()
            while time.time() - t0 < 90:
                st = d.rpc("getSignatureStatuses", [[sig], {"searchTransactionHistory": True}])
                val = (st.get("value") or [None])[0]
                if val:
                    if val.get("err"):
                        raise RuntimeError(f"tx err {val['err']}")
                    if val.get("confirmationStatus") in ("confirmed", "finalized"):
                        return sig
                time.sleep(0.4)
            raise TimeoutError("confirm")
        except Exception as e:
            last = e
            if "Blockhash" not in str(e) and "blockhash" not in str(e):
                raise
            time.sleep(0.3)
    raise last


def ix_accounts(ix: dict, keys: list[str]) -> list[str]:
    out = []
    for a in ix.get("accounts") or []:
        out.append(keys[a] if isinstance(a, int) else a)
    return out


def ix_program(ix: dict, keys: list[str]) -> str | None:
    pid = ix.get("programId")
    if pid is not None:
        return pid
    idx = ix.get("programIdIndex")
    if idx is not None and idx < len(keys):
        return keys[idx]
    return None


def latest_pump() -> dict:
    sigs = d.rpc("getSignaturesForAddress", [PAIR_PUMP, {"limit": 12}])
    for s in sigs:
        tx = d.rpc("getTransaction", [
            s["signature"],
            {"encoding": "json", "maxSupportedTransactionVersion": 1},
        ])
        if not tx or (tx.get("meta") or {}).get("err"):
            continue
        keys = d.tx_keys(tx)
        ixs = list(tx["transaction"]["message"].get("instructions") or [])
        for g in (tx.get("meta") or {}).get("innerInstructions") or []:
            ixs.extend(g.get("instructions") or [])
        for ix in ixs:
            if ix_program(ix, keys) != PUMP:
                continue
            accs = ix_accounts(ix, keys)
            raw = ix.get("data")
            data = d._b58_any(raw) if isinstance(raw, str) else bytes(raw or [])
            if accs[0] != PAIR_PUMP or len(data) < 8:
                continue
            # Live sell is 24 metas: fee_config @19, pfee @20, then extras.
            if len(accs) >= 24 and accs[20] == FEE_PROG:
                fee_cfg = accs[19] if exists(accs[19]) else FEE_CFG_LIVE
                extra = accs[21]
            elif len(accs) >= 22:
                fee_cfg = accs[21] if exists(accs[21]) else FEE_CFG_LIVE
                extra = accs[19] if accs[19] != fee_cfg else None
            else:
                continue
            return {
                "nacc": len(accs),
                "global": accs[2],
                "proto_fee": accs[9],
                "proto_fee_ata": accs[10],
                "event": accs[15],
                "creator_ata": accs[17],
                "creator_auth": accs[18],
                "slot19": extra,
                "fee_program": FEE_PROG,
                "fee_config": fee_cfg,
            }
    return pump_fallback()


def pump_fallback() -> dict:
    proto = "G5UZAVbAf46s7cKWoyKu8kYTip9DGTpbLZ2qa9Aq69dP"
    return {
        "global": d._pk(d.find_pda([b"global_config"], d.b58decode(PUMP))),
        "proto_fee": proto,
        "proto_fee_ata": ata(proto, SOL, TOKENKEG),
        "event": d._pk(d.find_pda([b"__event_authority"], d.b58decode(PUMP))),
        "creator_ata": "75mhQAcZeGKiL3wcuuVjTjg2SHJewZuQpd59ukiG24yv",
        "creator_auth": "F2Ne1XNs5f7LQNBdfB5WE69AGunwjAa6akdVGfVXiZxY",
        "slot19": None,
        "fee_program": FEE_PROG,
        "fee_config": FEE_CFG_LIVE,
    }


def resolve(wallet: str) -> dict:
    sn = d.snapshot_pool(PAIR_DLMM, [PAIR_DLMM], None)
    if not sn or not sn.get("lb"):
        raise SystemExit("DLMM snapshot failed")
    lb = sn["lb"]
    mint_x = d._pk(lb["token_x"])
    mint_y = d._pk(lb["token_y"])
    active_arr = d.bin_array_index(lb["active_id"])
    bins = []
    for i in (active_arr, active_arr + 1, active_arr - 1, active_arr + 2, active_arr - 2):
        pk = d.bin_array_pda(PAIR_DLMM, i)
        if pk not in bins and exists(pk):
            bins.append(pk)
        if len(bins) == 2:
            break
    if len(bins) < 2:
        raise SystemExit("need two distinct live bin arrays")
    oracle = d._pk(d.find_pda([b"oracle", d.b58decode(PAIR_DLMM)], d.b58decode(DLMM)))
    event_dlmm = d._pk(d.find_pda([b"__event_authority"], d.b58decode(DLMM)))
    pacc = d.get_multiple([PAIR_PUMP])[0]
    p = live.parse_pump_pool(pacc["data"]) if pacc else None
    if not p:
        raise SystemExit("pump parse failed")
    pump = latest_pump()
    fee_cfg = pump.get("fee_config")
    if not fee_cfg or not exists(fee_cfg):
        fee_cfg = FEE_CFG_LIVE
    gvol = d._pk(d.find_pda([b"global_volume_accumulator"], d.b58decode(PUMP)))
    pool_v2 = d._pk(d.find_pda([b"pool-v2", d.b58decode(mint_x)], d.b58decode(PUMP)))
    uvol = d._pk(d.find_pda(
        [b"user_volume_accumulator", d.b58decode(wallet)],
        d.b58decode(PUMP),
    ))
    fee_recipient = FEE_RECIPIENT
    fee_rec_quote = ata(fee_recipient, mint_y, TOKENKEG)
    if not exists(fee_rec_quote):
        for rec in FEE_RECIPIENTS:
            q = ata(rec, mint_y, TOKENKEG)
            if exists(q):
                fee_recipient, fee_rec_quote = rec, q
                break
        else:
            raise SystemExit("no live fee_recipient_quote WSOL ATA among the 8")
    return {
        "pair_dlmm": PAIR_DLMM,
        "pair_pump": PAIR_PUMP,
        "mint_x": mint_x,
        "mint_y": mint_y,
        "user_quote": ata(wallet, SOL, TOKENKEG),
        "user_base": ata(wallet, mint_x, TOKEN2022),
        "event_dlmm": event_dlmm,
        "oracle": oracle,
        "bins": bins,
        "vault_x": d._pk(lb["vault_x"]),
        "vault_y": d._pk(lb["vault_y"]),
        "vault_b": d._pk(p["vault_base"]),
        "vault_q": d._pk(p["vault_quote"]),
        "pump": pump,
        "fee_cfg": fee_cfg,
        "gvol": gvol,
        "pool_v2": pool_v2,
        "uvol": uvol,
        "fee_recipient": fee_recipient,
        "fee_rec_quote": fee_rec_quote,
        "active_id": lb["active_id"],
    }


def v0_parts(wallet: str, our_exec: str, acc: dict) -> tuple[list[str], list[str], list[str]]:
    static = [
        wallet, our_exec, CU_PROG, TOKENKEG, SYSTEM, ATA_PROG, MEMO,
        DLMM, PUMP, TOKEN2022, FEE_PROG,
    ]
    pair_dlmm = acc.get("pair_dlmm") or PAIR_DLMM
    pair_pump = acc.get("pair_pump") or PAIR_PUMP
    wr = [
        acc["user_quote"], acc["user_base"], pair_dlmm, acc["vault_x"], acc["vault_y"],
        acc["oracle"], acc["bins"][0], acc["bins"][1], pair_pump, acc["vault_b"],
        acc["vault_q"], acc["pump"]["proto_fee_ata"], acc["pump"]["creator_ata"],
        acc["uvol"],
    ]
    ro = [
        acc["event_dlmm"], acc["mint_x"], acc["mint_y"], acc["pump"]["event"],
        acc["pump"]["global"], acc["fee_cfg"], acc["pump"]["proto_fee"],
        acc["pump"]["creator_auth"], acc["pool_v2"],
    ]
    wr.append(acc["fee_rec_quote"])
    ro.append(acc["fee_recipient"])
    return static, wr, ro


def loaded_keys(static: list[str], wr: list[str], ro: list[str]) -> list[str]:
    return static + wr + ro


def logical35(loaded: list[str]) -> list[str]:
    return [loaded[IX_IDX[i]] for i in range(35)]


def compile_v0(static: list[str], alt: str,
               wr_ix: bytes | None = None, ro_ix: bytes | None = None) -> bytes:
    wr_ix = wr_ix or ALT_WR_IX
    ro_ix = ro_ix or ALT_RO_IX
    if len(wr_ix) != 15 or len(ro_ix) != 10:
        raise SystemExit(f"v0 alt ix wr={len(wr_ix)} ro={len(ro_ix)}")
    out = bytearray()
    out += bytes([1]) + bytes(64) + bytes([0x80, 1, 0, 10, 11])
    for k in static:
        out += d.b58decode(k)
    out += bytes(32)
    out += bytes([3])
    out += bytes([2, 0, 5, 2]) + struct.pack("<I", CU_LIMIT)
    out += bytes([2, 0, 9, 3]) + bytes(8)
    out += bytes([1, 60]) + IX_IDX + bytes([25]) + DISC + bytes(17)
    out += bytes([1]) + d.b58decode(alt)
    out += bytes([15]) + wr_ix
    out += bytes([10]) + ro_ix
    if len(out) != V0_TX_LEN:
        raise SystemExit(f"v0 len {len(out)} != {V0_TX_LEN}")
    if out[V0_OFF_DIR] != 0 or out[538:546] != DISC:
        raise SystemExit("v0 offset lock failed")
    return bytes(out)


def compile_v0_buy(static: list[str], alt: str,
                   wr_ix: bytes | None = None, ro_ix: bytes | None = None) -> bytes:
    if len(IX_IDX_BUY) != 62:
        raise SystemExit(f"IX_IDX_BUY {len(IX_IDX_BUY)}")
    wr_ix = wr_ix or ALT_WR_IX_BUY
    ro_ix = ro_ix or ALT_RO_IX_BUY
    if len(wr_ix) != 16 or len(ro_ix) != 10:
        raise SystemExit(f"v0 buy alt ix wr={len(wr_ix)} ro={len(ro_ix)}")
    out = bytearray()
    out += bytes([1]) + bytes(64) + bytes([0x80, 1, 0, 10, 11])
    for k in static:
        out += d.b58decode(k)
    out += bytes(32)
    out += bytes([3])
    out += bytes([2, 0, 5, 2]) + struct.pack("<I", CU_LIMIT)
    out += bytes([2, 0, 9, 3]) + bytes(8)
    out += bytes([1, 62]) + IX_IDX_BUY + bytes([25]) + DISC + bytes(17)
    out += bytes([1]) + d.b58decode(alt)
    out += bytes([16]) + wr_ix
    out += bytes([10]) + ro_ix
    if len(out) != V0_TX_LEN_BUY:
        raise SystemExit(f"v0 buy len {len(out)} != {V0_TX_LEN_BUY}")
    if out[V0_OFF_DIR_BUY] != 0 or out[540:548] != DISC:
        raise SystemExit("v0 buy offset lock failed")
    return bytes(out)


def patch(tmpl: bytes, direction: int, amount: int, min_profit: int,
          bh: bytes, price: int, buy: bool = False) -> bytes:
    off_dir = V0_OFF_DIR_BUY if buy else V0_OFF_DIR
    off_amt = V0_OFF_AMT_BUY if buy else V0_OFF_AMT
    off_min = V0_OFF_MIN_BUY if buy else V0_OFF_MIN
    out = bytearray(tmpl)
    out[V0_OFF_BH:V0_OFF_BH + 32] = bh
    out[V0_OFF_PRICE:V0_OFF_PRICE + 8] = struct.pack("<Q", price)
    out[off_dir] = direction
    out[off_amt:off_amt + 8] = struct.pack("<Q", amount)
    out[off_min:off_min + 8] = struct.pack("<Q", min_profit)
    return bytes(out)


def sign_v0(payer: Keypair, tx: bytes, buy: bool = False) -> bytes:
    msg_len = V0_MSG_LEN_BUY if buy else V0_MSG_LEN
    sig = bytes(payer.sign_message(tx[V0_OFF_MSG:V0_OFF_MSG + msg_len]))
    out = bytearray(tx)
    out[V0_OFF_SIG:V0_OFF_SIG + 64] = sig
    return bytes(out)


def alt_pda(authority: bytes, slot: int) -> tuple[bytes, int]:
    seeds = [authority, struct.pack("<Q", slot)]
    prog = d.b58decode(ALT_PROG)
    for bump in range(255, -1, -1):
        h = hashlib.sha256()
        for s in seeds:
            h.update(s)
        h.update(bytes([bump]))
        h.update(prog)
        h.update(b"ProgramDerivedAddress")
        digest = h.digest()
        if not d._on_curve(digest):
            return digest, bump
    raise SystemExit("alt pda")


def alt_addresses(pk: str) -> list[str]:
    acc = None
    for _ in range(8):
        acc = d.get_multiple([pk])[0]
        if acc and len(acc["data"]) >= 56:
            break
        time.sleep(0.4)
    if not acc or len(acc["data"]) < 56:
        return []
    raw = acc["data"]
    addrs = []
    for i in range(56, len(raw), 32):
        if i + 32 > len(raw):
            break
        addrs.append(d._pk(raw[i:i + 32]))
    return addrs


def publish_plane(alts: list[dict], routes: list[dict] | None = None) -> None:
    DEPLOY.mkdir(parents=True, exist_ok=True)
    payload = {
        "alts": [
            {"pubkey": a["pubkey"], "n": len(a["addresses"]), "addresses": a["addresses"]}
            for a in alts if a.get("pubkey") and a.get("addresses")
        ],
        "routes": routes or [],
    }
    tmp = PLANE.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload) + "\n", encoding="utf-8")
    tmp.replace(PLANE)


def alt_extend(payer: Keypair, pk: str, extra: list[str]) -> None:
    if not extra:
        return
    for off in range(0, len(extra), 20):
        chunk = extra[off:off + 20]
        payload = struct.pack("<IQ", 2, len(chunk)) + b"".join(
            d.b58decode(a) for a in chunk
        )
        eix = Instruction(
            Pubkey.from_string(ALT_PROG),
            payload,
            [
                AccountMeta(Pubkey.from_string(pk), False, True),
                AccountMeta(payer.pubkey(), True, False),
                AccountMeta(payer.pubkey(), True, True),
                AccountMeta(Pubkey.from_string(SYSTEM), False, False),
            ],
        )
        sig = send_ok(payer, [set_compute_unit_price(1), eix])
        print(f"  ALT extend {pk[:8]} {off}+{len(chunk)} {sig[:12]}..", flush=True)


def alt_create(payer: Keypair, table: list[str]) -> str:
    if not table or len(table) > 256:
        raise RuntimeError(f"alt_create n={len(table)}")
    slot = int(d.rpc("getSlot", [{"commitment": "confirmed"}]))
    raw, bump = alt_pda(bytes(payer.pubkey()), slot)
    alt = d._pk(raw)
    rent = d.rpc("getMinimumBalanceForRentExemption", [56 + 32 * len(table)])
    print(f"  create ALT slot={slot} n={len(table)} rent~{rent / 1e9:.4f} {alt}", flush=True)
    data = struct.pack("<IQB", 0, slot, bump)
    ix = Instruction(
        Pubkey.from_string(ALT_PROG),
        data,
        [
            AccountMeta(Pubkey.from_string(alt), False, True),
            AccountMeta(payer.pubkey(), True, False),
            AccountMeta(payer.pubkey(), True, True),
            AccountMeta(Pubkey.from_string(SYSTEM), False, False),
        ],
    )
    send_ok(payer, [set_compute_unit_price(1), ix])
    alt_extend(payer, alt, table)
    for _ in range(20):
        got = alt_addresses(alt)
        if got == table:
            return alt
        time.sleep(0.4)
    raise RuntimeError(f"ALT create mismatch {alt} have={len(alt_addresses(alt))}")


def native_balance(wallet: str) -> int:
    bal = d.rpc("getBalance", [wallet])
    return bal["value"] if isinstance(bal, dict) else int(bal)


def ensure_ata(payer: Keypair, owner: str, mint: str, tok: str) -> str:
    pk = ata(owner, mint, tok)
    if exists(pk):
        return pk
    ix = Instruction(
        Pubkey.from_string(ATA_PROG),
        bytes([1]),
        [
            AccountMeta(payer.pubkey(), True, True),
            AccountMeta(Pubkey.from_string(pk), False, True),
            AccountMeta(Pubkey.from_string(owner), False, False),
            AccountMeta(Pubkey.from_string(mint), False, False),
            AccountMeta(Pubkey.from_string(SYSTEM), False, False),
            AccountMeta(Pubkey.from_string(tok), False, False),
        ],
    )
    sig = send_ok(payer, [set_compute_unit_price(1), ix])
    print(f"  created ATA {pk[:8]}.. {sig[:12]}..")
    return pk


def wrap_wsol(payer: Keypair, wsol: str, lamports: int) -> None:
    info = d.get_multiple([wsol])[0]
    have = d.token_amount(info["data"]) if info else 0
    if have >= lamports:
        print(f"  wsol {have}")
        return
    need = lamports - have
    ix_tr = transfer(TransferParams(
        from_pubkey=payer.pubkey(),
        to_pubkey=Pubkey.from_string(wsol),
        lamports=need,
    ))
    ix_sync = Instruction(
        Pubkey.from_string(TOKENKEG),
        bytes([17]),
        [AccountMeta(Pubkey.from_string(wsol), False, True)],
    )
    sig = send_ok(payer, [ix_tr, ix_sync])
    print(f"  wrap {need} {sig[:12]}..")


def load_alt_plane() -> list[dict]:
    if not PLANE.exists():
        return []
    obj = json.loads(PLANE.read_text(encoding="utf-8"))
    return obj.get("alts") or []


def lookup_prepared(wr: list[str], ro: list[str]) -> tuple[str, bytes, bytes]:
    need = wr + ro
    for alt in load_alt_plane():
        have = alt.get("addresses") or []
        pos = {a: i for i, a in enumerate(have)}
        if any(a not in pos for a in need):
            continue
        return alt["pubkey"], bytes(pos[a] for a in wr), bytes(pos[a] for a in ro)
    missing = []
    if load_alt_plane():
        have = set()
        for alt in load_alt_plane():
            have.update(alt.get("addresses") or [])
        missing = [a[:8] for a in need if a not in have][:6]
    raise AltPlaneMiss(f"no prepared ALT covers wr={len(wr)} ro={len(ro)} miss={missing}")


def ensure_alt(payer: Keypair, table: list[str]) -> str:
    path = DEPLOY / "alt.json"
    if path.exists():
        obj = json.loads(path.read_text(encoding="utf-8"))
        pk = obj.get("pubkey")
        if pk and exists(pk):
            have = alt_addresses(pk)
            if have == table:
                print(f"  ALT reuse {pk}")
                return pk
            if have and len(have) == len(table):
                pinned = list(table)
                pinned[6], pinned[7] = have[6], have[7]
                if pinned == have:
                    print(f"  ALT reuse (pin bins) {pk}")
                    return pk
            if have and table[:len(have)] == have and len(table) > len(have):
                extra = table[len(have):]
                print(f"  ALT extend {pk} +{len(extra)} (no new rent)")
                for off in range(0, len(extra), 20):
                    chunk = extra[off:off + 20]
                    payload = struct.pack("<IQ", 2, len(chunk)) + b"".join(
                        d.b58decode(a) for a in chunk
                    )
                    eix = Instruction(
                        Pubkey.from_string(ALT_PROG),
                        payload,
                        [
                            AccountMeta(Pubkey.from_string(pk), False, True),
                            AccountMeta(payer.pubkey(), True, False),
                            AccountMeta(payer.pubkey(), True, True),
                            AccountMeta(Pubkey.from_string(SYSTEM), False, False),
                        ],
                    )
                    sig = send_ok(payer, [set_compute_unit_price(1), eix])
                    print(f"  ALT extend {off}+{len(chunk)} {sig[:12]}..")
                path.write_text(
                    json.dumps({"pubkey": pk, "n": len(table), "extended": extra}) + "\n",
                    encoding="utf-8",
                )
                got = []
                for _ in range(20):
                    got = alt_addresses(pk)
                    if got == table:
                        return pk
                    time.sleep(0.5)
                raise SystemExit(f"ALT extend mismatch have={len(got)} want={len(table)}")
            print(f"  ALT stale {pk[:8]}.. have={len(have)} want={len(table)}")
            for i, (a, b) in enumerate(zip(have, table)):
                if a != b:
                    print(f"    mismatch [{i}] have={a[:8]} want={b[:8]}")
                    if i >= 2:
                        break
            if os.environ.get("SIM_NO_NEW_ALT") == "1":
                print(f"  ALT reuse stale {pk} (SIM_NO_NEW_ALT)")
                return pk
            print(f"  ALT stale {pk[:8]}.. — create new")
    slot = int(d.rpc("getSlot", [{"commitment": "confirmed"}]))
    raw, bump = alt_pda(bytes(payer.pubkey()), slot)
    alt = d._pk(raw)
    rent = d.rpc("getMinimumBalanceForRentExemption", [56 + 32 * len(table)])
    print(f"  create ALT slot={slot} rent~{rent / 1e9:.4f} {alt}")
    data = struct.pack("<IQB", 0, slot, bump)
    ix = Instruction(
        Pubkey.from_string(ALT_PROG),
        data,
        [
            AccountMeta(Pubkey.from_string(alt), False, True),
            AccountMeta(payer.pubkey(), True, False),
            AccountMeta(payer.pubkey(), True, True),
            AccountMeta(Pubkey.from_string(SYSTEM), False, False),
        ],
    )
    sig = send_ok(payer, [set_compute_unit_price(1), ix])
    print(f"  ALT create {sig[:12]}..")
    for off in range(0, len(table), 20):
        chunk = table[off:off + 20]
        payload = struct.pack("<IQ", 2, len(chunk)) + b"".join(d.b58decode(a) for a in chunk)
        eix = Instruction(
            Pubkey.from_string(ALT_PROG),
            payload,
            [
                AccountMeta(Pubkey.from_string(alt), False, True),
                AccountMeta(payer.pubkey(), True, False),
                AccountMeta(payer.pubkey(), True, True),
                AccountMeta(Pubkey.from_string(SYSTEM), False, False),
            ],
        )
        sig = send_ok(payer, [set_compute_unit_price(1), eix])
        print(f"  ALT extend {off}+{len(chunk)} {sig[:12]}..")
    DEPLOY.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"pubkey": alt, "n": len(table), "slot": slot}) + "\n", encoding="utf-8")
    got = []
    for _ in range(20):
        got = alt_addresses(alt)
        if got == table:
            return alt
        time.sleep(0.5)
    raise SystemExit(f"ALT contents mismatch have={len(got)} want={len(table)}")


def simulate(tx: bytes) -> dict:
    raw = base64.b64encode(tx).decode()
    print(f"  raw={len(tx)} b64={len(raw)} wire=1232 margin={1232 - len(tx)}")
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
        msg = str(e)
        print(f"  simulate rpc {msg[:240]}")
        return {"err": "rpc", "rpc": msg, "cu": None, "logs": [], "inner": 0, "accepted": False}
    val = res.get("value") or res
    err = val.get("err")
    cu = val.get("unitsConsumed")
    logs = val.get("logs") or []
    print(f"  simulate err={err} cu={cu} logs={len(logs)}")
    for line in logs[:16]:
        print("   ", line[:180])
    if len(logs) > 16:
        print("    ...")
        print("   ", logs[-1][:180])
    return {
        "err": err,
        "cu": cu,
        "logs": logs[-24:],
        "inner": len(val.get("innerInstructions") or []),
        "accepted": True,
    }


def inspect_logical(keys: list[str]) -> list[dict]:
    accs = d.get_multiple(keys)
    rows = []
    for i, k in enumerate(keys):
        a = accs[i]
        rows.append({
            "i": i,
            "name": NAMES[i],
            "pubkey": k,
            "exists": bool(a),
            "owner": (a or {}).get("owner"),
            "dlen": len(a["data"]) if a else 0,
        })
    return rows


def main() -> int:
    live.load_dotenv()
    payer = payer_kp()
    wallet = str(payer.pubkey())
    our_exec = program_v3()
    bal = d.rpc("getBalance", [wallet])
    lamports = bal["value"] if isinstance(bal, dict) else bal
    print(f"EXEC-LIVE-002B  wallet={wallet}  sol={lamports / 1e9:.6f}")
    print(f"  OUR_EXEC={our_exec}")

    acc = resolve(wallet)
    print(f"  active={acc['active_id']} fee_cfg={acc['fee_cfg'][:8]}.. uvol={acc['uvol'][:8]}..")
    ensure_ata(payer, wallet, SOL, TOKENKEG)
    ensure_ata(payer, wallet, acc["mint_x"], TOKEN2022)
    wrap_wsol(payer, acc["user_quote"], TINY_IN)

    direction = int(os.environ.get("SIM_DIR", "0"))
    if direction not in (0, 1):
        raise SystemExit(f"SIM_DIR {direction}")
    static, wr, ro = v0_parts(wallet, our_exec, acc)
    if direction == 1:
        wr.append(acc["gvol"])
        table = wr[:14] + ro[:9] + [wr[14], ro[9], wr[15]]
        ix_map = IX_IDX_BUY
    else:
        table = wr[:14] + ro[:9] + [wr[14], ro[9]]
        ix_map = IX_IDX
    print(f"  fee_rec={acc['fee_recipient'][:8]}.. quote_ata={acc['fee_rec_quote'][:8]}..")
    dups = [x for x in table if table.count(x) > 1]
    if dups:
        raise SystemExit(f"ALT duplicate {dups[0][:8]}")
    overlap = set(table) & set(static)
    if overlap:
        raise SystemExit(f"ALT overlaps static {next(iter(overlap))[:8]}")
    loaded = loaded_keys(static, wr, ro)
    logical = [loaded[ix_map[i]] for i in range(35)]
    if logical[0] != wallet or logical[7] != DLMM or logical[19] != PUMP:
        raise SystemExit("logical order lock failed")
    if logical[10] != DLMM or logical[14] != DLMM:
        raise SystemExit("optional-none bitmap/host must be DLMM program")
    if logical[15] != logical[25] or logical[16] != logical[26]:
        raise SystemExit("mint alias lock failed")
    print(f"  static={len(static)} alt_wr={len(wr)} alt_ro={len(ro)} loaded={len(loaded)}")

    rows = inspect_logical(logical)
    missing = [r for r in rows if not r["exists"]]
    print(f"  logical exist={35 - len(missing)}/35  missing={len(missing)}")
    for r in missing:
        print(f"    missing [{r['i']:02}] {r['name']} {r['pubkey'][:8]}..")

    alt = ensure_alt(payer, table)
    buy = direction == 1
    tmpl = compile_v0_buy(static, alt) if buy else compile_v0(static, alt)
    bh = d.b58decode(d.rpc("getLatestBlockhash", [{"commitment": "confirmed"}])["value"]["blockhash"])
    tiny = int(os.environ.get("SIM_IN", "0")) or (50_000_000 if buy else TINY_IN)
    print(f"  sim_dir={direction} tiny_in={tiny} min_guard={MIN_GUARD}")
    tx = sign_v0(payer, patch(tmpl, direction, tiny, MIN_GUARD, bh, 1, buy=buy), buy=buy)
    if tx[0] != 1 or tx[65] != 0x80:
        raise SystemExit("v0 header")
    print(f"  signed v0 {len(tx)} B")

    sim = simulate(tx)
    exec_acc = d.get_multiple([our_exec])[0]

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "signed.tx").write_bytes(tx)
    (OUT / "keys.json").write_text(json.dumps(
        [{"i": i, "name": NAMES[i], "pubkey": logical[i]} for i in range(35)],
        indent=2,
    ) + "\n", encoding="utf-8")
    report = {
        "wallet": wallet,
        "our_exec": our_exec,
        "our_exec_executable": bool(exec_acc and exec_acc.get("executable")),
        "alt": alt,
        "tx_len": len(tx),
        "wire_max": 1232,
        "margin": 1232 - len(tx),
        "tiny_in": TINY_IN,
        "min_profit": MIN_GUARD,
        "optional_none": {"bitmap": DLMM, "host_fee": DLMM},
        "fee_cfg": acc["fee_cfg"],
        "fee_recipient": acc["fee_recipient"],
        "fee_recipient_quote": acc["fee_rec_quote"],
        "logical_order_unchanged": True,
        "missing": missing,
        "accounts": rows,
        "simulate": sim,
        "note": "OUR_EXEC not on-chain until loader-v3 deploy. Packet-accept is the 002B gate.",
    }
    (OUT / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT}  margin={1232 - len(tx)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
