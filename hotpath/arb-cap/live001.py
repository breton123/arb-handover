#!/usr/bin/env python3
"""LIVE-001 control plane. RPC stays here. Writes liveuniv.bin for arb-core.

Discovers current mainnet DLMM + Pump pools, fetches LbPair / bins / vaults
and Pump reserves + fees, assigns dense pool_idx order, writes a snapshot.

Env: HELIUS_API_KEY or HELIUS_RPC_URL. Never prints the key.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import record_dlmm as d  # noqa: E402

PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
SOL = "So11111111111111111111111111111111111111112"
LIVE_MAGIC = 0x4530314C
LIVE_VER = 1
LIVE_VER2 = 2
PROTO_DLMM = 1
PROTO_PUMP = 2
PROTO_CPMM = 4
USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
USDT = "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"
CPMM = "CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C"
K = 16


def load_dotenv() -> None:
    roots = [
        Path(__file__).resolve().parents[1] / ".env",
        Path.home() / ".arb-smoke.env",
        Path.home() / "Desktop" / "ArbResearch" / ".env",
        Path(r"C:\Users\louis\Desktop\ArbResearch") / ".env",
        Path(r"C:\Users\louis\Desktop\ArbResearch") / "atomic_arbitrage" / ".env",
        Path(r"C:\Users\louis\Desktop\ArbResearch") / "atomic_arbitrage" / "long_hop_analysis" / ".env",
    ]
    for p in roots:
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if not s or s.startswith("#") or "=" not in s:
                continue
            if s.startswith("export "):
                s = s[7:].strip()
            k, v = s.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
    if not os.environ.get("HELIUS_RPC_URL") and os.environ.get("RPC_URL"):
        os.environ["HELIUS_RPC_URL"] = os.environ["RPC_URL"]


def b58e(raw: bytes) -> str:
    return d._pk(raw)


def b58d(s: str) -> bytes:
    return d.b58decode(s)


def pump_ix_pools(tx: dict) -> list[str]:
    if not tx or (tx.get("meta") or {}).get("err"):
        return []
    keys = d.tx_keys(tx)
    msg = tx["transaction"]["message"]
    ixs = list(msg.get("instructions") or [])
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        ixs.extend(g.get("instructions") or [])
    out = []
    for ix in ixs:
        pid = ix.get("programId")
        if pid is None:
            idx = ix.get("programIdIndex")
            pid = keys[idx] if idx is not None and idx < len(keys) else None
        if pid != PUMP:
            continue
        accs = []
        for a in ix.get("accounts") or []:
            accs.append(keys[a] if isinstance(a, int) else a)
        if accs:
            out.append(accs[0])
    return out


def recent_program_pools(program: str, decode, want: int, scan: int) -> list[str]:
    sigs = d.rpc("getSignaturesForAddress", [program, {"limit": scan}])
    seen: list[str] = []
    have = set()
    for s in sigs:
        tx = d.rpc(
            "getTransaction",
            [s["signature"], {"encoding": "json", "maxSupportedTransactionVersion": 1}],
        )
        for pk in decode(tx):
            if pk in have:
                continue
            have.add(pk)
            seen.append(pk)
            if len(seen) >= want:
                return seen
    return seen


def dlmm_from_tx(tx: dict) -> list[str]:
    sw = d.decode_swaps(tx) if tx else []
    return [x["pair"] for x in sw]


def parse_pump_pool(data: bytes) -> dict | None:
    if len(data) < 211:
        return None
    base = data[43:75]
    quote = data[75:107]
    vault_b = data[139:171]
    vault_q = data[171:203]
    mayhem = 0
    cashback = 0
    virtual = 0
    if len(data) > 243:
        mayhem = data[243]
    if len(data) > 244:
        cashback = data[244]
    # Pool.virtual_quote_reserves i128 at 245. Absent on legacy accounts → 0.
    if len(data) >= 261:
        virtual = int.from_bytes(data[245:261], "little", signed=True)
        if virtual > (2**63 - 1) or virtual < -(2**63):
            return None
    creator = data[11:43] if len(data) >= 43 else b""
    coin_creator = data[211:243] if len(data) >= 243 else b""
    pool_cr = int.from_bytes(data[261:269], "little") if len(data) >= 269 else 0
    return {
        "base": base,
        "quote": quote,
        "vault_base": vault_b,
        "vault_quote": vault_q,
        "mayhem": mayhem,
        "cashback": cashback,
        "virtual_quote_reserves": int(virtual),
        "creator": creator,
        "coin_creator": coin_creator,
        "creator_fee_bps": pool_cr,
        "holder": data[270] if len(data) > 270 else 0,
    }


def parse_mint_supply(data: bytes) -> int:
    """SPL / Token-2022 mint: supply u64 at offset 36."""
    if not data or len(data) < 44:
        return 0
    return int.from_bytes(data[36:44], "little")


def parse_global(data: bytes) -> tuple[int, int, int, int]:
    if len(data) < 57:
        return 20, 5, 0, 0
    lp = struct.unpack_from("<Q", data, 40)[0]
    proto = struct.unpack_from("<Q", data, 48)[0]
    disabled = data[56]
    creator = 0
    if len(data) >= 57 + 32 * 8 + 8:
        creator = struct.unpack_from("<Q", data, 57 + 32 * 8)[0]
    if lp > 10000 or proto > 10000 or creator > 10000:
        return 20, 5, 0, disabled
    return int(lp), int(proto), int(creator), int(disabled)


def write_u8(f, v: int) -> None:
    f.write(struct.pack("<B", v))


def write_u16(f, v: int) -> None:
    f.write(struct.pack("<H", v))


def write_u32(f, v: int) -> None:
    f.write(struct.pack("<I", v))


def write_u64(f, v: int) -> None:
    f.write(struct.pack("<Q", v))


def write_i32(f, v: int) -> None:
    f.write(struct.pack("<i", v))


def write_i64(f, v: int) -> None:
    f.write(struct.pack("<q", v))


def write_dlmm(f, snap: dict) -> None:
    lb = snap["lb"]
    active = lb["active_id"]
    window = [b for b in snap["bins"] if abs(b["id"] - active) <= K]
    seen = set()
    bins = []
    for b in window:
        if b["id"] in seen:
            continue
        seen.add(b["id"])
        bins.append(b)
    bins.sort(key=lambda x: x["id"])
    write_i32(f, active)
    write_u16(f, lb["bin_step"])
    write_u8(f, lb["status"])
    write_u16(f, lb["base_factor"])
    write_u16(f, lb["filter_period"])
    write_u16(f, lb["decay_period"])
    write_u16(f, lb["reduction_factor"])
    write_u32(f, lb["variable_fee_control"])
    write_u32(f, lb["max_volatility_accumulator"])
    write_u16(f, lb["protocol_share"])
    write_u8(f, lb["base_fee_power_factor"])
    write_u8(f, lb["collect_fee_mode"])
    write_u32(f, lb["vol_acc"])
    write_u32(f, lb["vol_ref"])
    write_i32(f, lb["idx_ref"])
    write_i64(f, lb["last_upd"])
    write_u64(f, snap["reserve_x"])
    write_u64(f, snap["reserve_y"])
    write_i64(f, int(time.time()))
    write_u16(f, len(bins))
    for b in bins:
        write_i32(f, b["id"])
        write_u64(f, b["x"])
        write_u64(f, b["y"])


def write_amm2(f, st: dict) -> None:
    write_u64(f, int(st["reserve_x"]))
    write_u64(f, int(st["reserve_y"]))
    write_u16(f, int(st.get("fee_bps") or 25))
    write_u8(f, int(st.get("status") or 0))


def write_pump(f, st: dict) -> None:
    write_u64(f, st["reserve_base"])
    write_u64(f, st["reserve_quote"])
    write_i64(f, st["virtual_quote"])
    write_u64(f, st["lp_fee_bps"])
    write_u64(f, st["protocol_fee_bps"])
    write_u64(f, st["creator_fee_bps"])
    write_u8(f, st["disabled"])
    write_u8(f, st["status"])


def write_meta(f, proto: int, pubkey: bytes, mx, my, vx, vy) -> None:
    write_u8(f, proto)
    f.write(pubkey)
    f.write(mx)
    f.write(my)
    f.write(vx)
    f.write(vy)


def fetch_pump(pk: str, fees: tuple[int, int, int, int]) -> dict | None:
    accs = d.get_multiple([pk])
    a = accs[0] if accs else None
    if a is None or a["owner"] != PUMP:
        return None
    p = parse_pump_pool(a["data"])
    if p is None:
        return None
    vaults = d.get_multiple([b58e(p["vault_base"]), b58e(p["vault_quote"])])
    rb = d.token_amount(vaults[0]["data"]) if vaults[0] else 0
    rq = d.token_amount(vaults[1]["data"]) if vaults[1] else 0
    if rb == 0 or rq == 0:
        return None
    lp, proto, creator, disabled = fees
    return {
        "pubkey": b58d(pk),
        "mint_x": p["base"],
        "mint_y": p["quote"],
        "vault_x": p["vault_base"],
        "vault_y": p["vault_quote"],
        "reserve_base": rb,
        "reserve_quote": rq,
        "virtual_quote": int(p.get("virtual_quote_reserves") or 0),
        "base_vault_amount": rb,
        "quote_vault_amount": rq,
        "virtual_quote_reserves": int(p.get("virtual_quote_reserves") or 0),
        "lp_fee_bps": lp,
        "protocol_fee_bps": proto,
        "creator_fee_bps": creator,
        "disabled": disabled,
        "status": 1 if (p["mayhem"] or p.get("cashback")) else 0,
        "slot": a["slot"],
        "token": b58e(p["base"]) if b58e(p["quote"]) == SOL else b58e(p["quote"]),
        "sol_side": b58e(p["quote"]) == SOL or b58e(p["base"]) == SOL,
    }


def fetch_cpmm(pk: str) -> dict | None:
    accs = d.get_multiple([pk], retries=2)
    a = accs[0] if accs else None
    if a is None or a.get("owner") != CPMM or not a.get("data") or len(a["data"]) < 300:
        return None
    data = a["data"]
    mx, my = data[72:104], data[104:136]
    vx, vy = data[168:200], data[200:232]
    vs = d.get_multiple([b58e(vx), b58e(vy)], retries=2)
    rx = d.token_amount(vs[0]["data"]) if vs and vs[0] and vs[0].get("data") else 0
    ry = d.token_amount(vs[1]["data"]) if vs and vs[1] and vs[1].get("data") else 0
    if rx == 0 or ry == 0:
        return None
    return {
        "pubkey": b58d(pk),
        "mint_x": mx,
        "mint_y": my,
        "vault_x": vx,
        "vault_y": vy,
        "reserve_x": rx,
        "reserve_y": ry,
        "fee_bps": 25,
        "status": 0,
        "slot": int(a.get("slot") or 0),
        "token": b58e(mx) if b58e(my) in (SOL, USDC, USDT) else b58e(my),
        "sol_side": b58e(mx) == SOL or b58e(my) == SOL,
    }


def fetch_dlmm(pk: str) -> dict | None:
    sn = d.snapshot_pool(pk, [pk], None)
    if sn is None or not sn.get("lb"):
        return None
    lb = sn["lb"]
    window = [b for b in sn["bins"] if abs(b["id"] - lb["active_id"]) <= K]
    if not window:
        return None
    return {
        "snap": sn,
        "pubkey": b58d(pk),
        "mint_x": lb["token_x"],
        "mint_y": lb["token_y"],
        "vault_x": lb["vault_x"],
        "vault_y": lb["vault_y"],
        "slot": sn["slot"],
        "token": b58e(lb["token_x"]) if b58e(lb["token_y"]) == SOL else b58e(lb["token_y"]),
        "sol_side": b58e(lb["token_x"]) == SOL or b58e(lb["token_y"]) == SOL,
    }


def main() -> int:
    load_dotenv()
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "live001" / "liveuniv.bin"))
    ap.add_argument("--want", type=int, default=12)
    ap.add_argument("--scan", type=int, default=40)
    ap.add_argument("--sig", action="append", default=[],
                    help="seed DLMM+Pump pools from this tx (repeatable)")
    args = ap.parse_args()

    print("LIVE-001  discover (rpc url redacted)", flush=True)
    gcfg_pk = d._pk(d.find_pda([b"global_config"], b58d(PUMP)))
    gacc = d.get_multiple([gcfg_pk])[0]
    fees = parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)
    print(f"  pump fees lp={fees[0]} proto={fees[1]} creator={fees[2]}", flush=True)

    def gpa_dlmm(token: str, token_is_x: bool) -> list[str]:
        off = 88 if token_is_x else 120
        sol_off = 120 if token_is_x else 88
        try:
            res = d.rpc(
                "getProgramAccounts",
                [
                    d.DLMM,
                    {
                        "encoding": "base64",
                        "filters": [
                            {"memcmp": {"offset": off, "bytes": token}},
                            {"memcmp": {"offset": sol_off, "bytes": SOL}},
                        ],
                    },
                ],
            )
        except Exception as e:
            print(f"  gPA skip {token[:8]}: {type(e).__name__}", flush=True)
            return []
        out = []
        for acc in res or []:
            pk = acc.get("pubkey")
            if pk:
                out.append(pk)
        return out

    seed_d: list[str] = []
    seed_p: list[str] = []
    for sig in args.sig:
        tx = d.rpc(
            "getTransaction",
            [sig, {"encoding": "json", "maxSupportedTransactionVersion": 1}],
        )
        seed_d.extend(dlmm_from_tx(tx))
        seed_p.extend(pump_ix_pools(tx))
        print(f"  seed {sig[:8]}.. dlmm={seed_d[-1][:8] if seed_d else '-'} "
              f"pump={seed_p[-1][:8] if seed_p else '-'}", flush=True)

    dlmm_pks = list(dict.fromkeys(seed_d + recent_program_pools(d.DLMM, dlmm_from_tx, args.want, args.scan)))
    pump_pks = list(dict.fromkeys(seed_p + recent_program_pools(PUMP, pump_ix_pools, args.want, args.scan)))
    print(f"  found dlmm={len(dlmm_pks)} pump={len(pump_pks)}", flush=True)

    dlmm_rows = []
    for pk in dlmm_pks:
        row = fetch_dlmm(pk)
        if row is None:
            print(f"  skip dlmm {pk[:8]}", flush=True)
            continue
        dlmm_rows.append(row)
        print(
            f"  dlmm {pk[:8]}.. active={row['snap']['lb']['active_id']} "
            f"bins={len(row['snap']['bins'])} rx={row['snap']['reserve_x']} "
            f"ry={row['snap']['reserve_y']}",
            flush=True,
        )

    pump_rows = []
    for pk in pump_pks:
        row = fetch_pump(pk, fees)
        if row is None:
            print(f"  skip pump {pk[:8]}", flush=True)
            continue
        pump_rows.append(row)
        print(
            f"  pump {pk[:8]}.. base={row['reserve_base']} quote={row['reserve_quote']}",
            flush=True,
        )

    if not dlmm_rows or not pump_rows:
        print("need at least one real DLMM and one real Pump", file=sys.stderr)
        return 1

    d_tokens = {r["token"] for r in dlmm_rows if r["sol_side"]}
    p_tokens = {r["token"] for r in pump_rows if r["sol_side"]}
    both = d_tokens & p_tokens
    if not both:
        print("  no overlap yet; gPA DLMM for Pump SOL tokens", flush=True)
        have_d = {b58e(r["pubkey"]) for r in dlmm_rows}
        for tok in list(p_tokens)[:6]:
            extra = gpa_dlmm(tok, True) + gpa_dlmm(tok, False)
            for pk in extra[:2]:
                if pk in have_d:
                    continue
                row = fetch_dlmm(pk)
                if row is None:
                    continue
                dlmm_rows.append(row)
                have_d.add(pk)
                print(f"  dlmm-gPA {pk[:8]}.. token={tok[:8]}", flush=True)
                if row["sol_side"] and row["token"] in p_tokens:
                    both.add(row["token"])
                    break
            if both:
                break
    print(f"  SOL-sided overlap tokens={len(both)}", flush=True)

    # Prefer overlapping route0 pairs, then fill with the rest.
    ordered = []
    used_d = set()
    used_p = set()
    for tok in both:
        for r in dlmm_rows:
            if r["sol_side"] and r["token"] == tok and id(r) not in used_d:
                ordered.append(("dlmm", r))
                used_d.add(id(r))
                break
        for r in pump_rows:
            if r["sol_side"] and r["token"] == tok and id(r) not in used_p:
                ordered.append(("pump", r))
                used_p.add(id(r))
                break
    for r in dlmm_rows:
        if id(r) not in used_d:
            ordered.append(("dlmm", r))
            used_d.add(id(r))
    for r in pump_rows:
        if id(r) not in used_p:
            ordered.append(("pump", r))
            used_p.add(id(r))

    if len(ordered) > 65535:
        raise SystemExit(f"n={len(ordered)} exceeds uint16 live.bin hard max")

    slot = max((r["slot"] for _, r in ordered), default=0)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        write_u32(f, LIVE_MAGIC)
        write_u16(f, LIVE_VER)
        write_u16(f, len(ordered))
        write_u64(f, slot)
        write_u64(f, slot if slot else 1)
        for kind, r in ordered:
            if kind == "dlmm":
                write_meta(f, PROTO_DLMM, r["pubkey"], r["mint_x"], r["mint_y"], r["vault_x"], r["vault_y"])
                write_dlmm(f, r["snap"])
            else:
                write_meta(f, PROTO_PUMP, r["pubkey"], r["mint_x"], r["mint_y"], r["vault_x"], r["vault_y"])
                write_pump(f, r)

    meta = {
        "path": str(out),
        "n": len(ordered),
        "n_dlmm": sum(1 for k, _ in ordered if k == "dlmm"),
        "n_pump": sum(1 for k, _ in ordered if k == "pump"),
        "overlap_tokens": len(both),
        "slot": slot,
        "pools": [
            {
                "idx": i,
                "proto": kind,
                "pubkey": b58e(r["pubkey"]),
                "sol_side": r["sol_side"],
            }
            for i, (kind, r) in enumerate(ordered)
        ],
    }
    (out.parent / "liveuniv.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}  n={len(ordered)}  slot={slot}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
