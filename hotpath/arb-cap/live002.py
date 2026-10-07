#!/usr/bin/env python3
"""LIVE-002 — six-venue universe from the 171k trial arbs.

Seeds pools from successful MEV.live rows (dex names + mints → live txs →
pool pubkeys), fetches compact state, writes liveuniv v2. No 8-token seed.
RPC stays here. Never prints secrets.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

SOL = "So11111111111111111111111111111111111111112"
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
CLMM = "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK"
CPMM = "CPMMoo8L3F4NbTegBCKVNunggL7H1ZpdTHKxQB5qKP1C"
DAMM = "cpamdpZCGKUy5JxQXB4dcpGPiikHawvSWAd6mEn1sGG"
ORCA = "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc"

PROTO = {DLMM: 1, PUMP: 2, CLMM: 3, CPMM: 4, DAMM: 5, ORCA: 6}
WANT = {DLMM, PUMP, CLMM, CPMM, DAMM, ORCA}
FAM = {
    "Meteora DLMM": DLMM,
    "Pump Swap": PUMP,
    "Raydium Concentrated Liquidity": CLMM,
    "Raydium CPMM": CPMM,
    "Meteora DAMM V2": DAMM,
    "Orca Whirlpools": ORCA,
}

LIVE_MAGIC = 0x4530314C
LIVE_VER = 2
POOL_MAX = 256

ORCA_TICK_ARRAY = 88
CLMM_TICK_ARRAY = 60


def our_row(r: dict) -> bool:
    dexes = r.get("dexes") or []
    if not dexes:
        return False
    return all(x in FAM for x in dexes)


def load_seeds(path: Path, top_tx: int) -> list[dict]:
    rows = []
    for line in path.open(encoding="utf-8"):
        r = json.loads(line)
        if not our_row(r):
            continue
        gp = float(r.get("pure_profit") or r.get("profit_usd") or 0)
        rows.append((gp, r))
    rows.sort(key=lambda x: -x[0])
    return [r for _, r in rows[:top_tx]]


TOKENKEG = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKENZ = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
SYSTEM = "11111111111111111111111111111111"
NOT_POOL = WANT | {TOKENKEG, TOKENZ, SYSTEM, SOL}

# Per-venue cap so DLMM $ rank cannot crowd out CLMM/CPMM/DAMM/Orca.
QUOTA = {"dlmm": 90, "pump": 40, "clmm": 40, "cpmm": 40, "damm": 23, "orca": 23}


def ix_accounts(tx: dict) -> list[tuple[str, str]]:
    """Every account on every wanted-program ix. Pool is rarely accounts[0]."""
    if not tx:
        return []
    keys = d.tx_keys(tx)
    ixs = list(tx["transaction"]["message"].get("instructions") or [])
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        ixs.extend(g.get("instructions") or [])
    out = []
    for ix in ixs:
        pid = ix.get("programId")
        if pid is None:
            idx = ix.get("programIdIndex")
            pid = keys[idx] if idx is not None and idx < len(keys) else None
        if pid not in WANT:
            continue
        for a in ix.get("accounts") or []:
            pk = keys[a] if isinstance(a, int) else a
            if pk and pk not in NOT_POOL:
                out.append((pid, pk))
    return out


def find_mint(data: bytes, mint: bytes) -> int:
    for i in range(0, max(0, len(data) - 31)):
        if data[i:i + 32] == mint:
            return i
    return -1


def parse_amm2_pair(data: bytes, mint_a_off: int, mint_b_off: int,
                    vault_a_off: int, vault_b_off: int) -> dict | None:
    if max(mint_a_off, mint_b_off, vault_a_off, vault_b_off) + 32 > len(data):
        return None
    return {
        "mint_x": data[mint_a_off:mint_a_off + 32],
        "mint_y": data[mint_b_off:mint_b_off + 32],
        "vault_x": data[vault_a_off:vault_a_off + 32],
        "vault_y": data[vault_b_off:vault_b_off + 32],
    }


def parse_clmm(data: bytes) -> dict | None:
    if len(data) < 300:
        return None
    # disc(8)+bump(1)+amm_config(32)+owner(32) → mint0 @73
    p = parse_amm2_pair(data, 73, 105, 137, 169)
    if p is None:
        return None
    liq = struct.unpack_from("<Q", data, 8 + 1 + 32 + 32 + 32 + 32 + 32 + 32 + 32 + 1 + 1 + 2)[0]
    # liquidity u128 @ 213? verify via known fields
    # After mint decimals + tick_spacing: liquidity u128, sqrt u128, tick i32
    off = 73 + 32 + 32 + 32 + 32 + 32 + 1 + 1 + 2  # 237
    if off + 16 + 16 + 4 > len(data):
        return None
    liq = struct.unpack_from("<Q", data, off)[0]
    sqrt = struct.unpack_from("<Q", data, off + 16)[0]
    tick = struct.unpack_from("<i", data, off + 32)[0]
    spacing = struct.unpack_from("<H", data, off - 2)[0]
    p.update({"liquidity": liq, "sqrt_price_x64": sqrt, "tick": tick,
              "tick_spacing": spacing or 1, "fee_bps": 25})
    return p


def parse_orca(data: bytes) -> dict | None:
    if len(data) < 250:
        return None
    spacing = struct.unpack_from("<H", data, 41)[0]
    fee_rate = struct.unpack_from("<H", data, 45)[0]
    liq = struct.unpack_from("<Q", data, 49)[0]
    sqrt = struct.unpack_from("<Q", data, 65)[0]
    tick = struct.unpack_from("<i", data, 81)[0]
    return {
        "mint_x": data[101:133],
        "mint_y": data[181:213],
        "vault_x": data[133:165],
        "vault_y": data[213:245],
        "liquidity": liq,
        "sqrt_price_x64": sqrt,
        "tick": tick,
        "tick_spacing": spacing or 1,
        "fee_bps": max(1, fee_rate // 100),
    }


def parse_cpmm(data: bytes) -> dict | None:
    if len(data) < 300:
        return None
    # disc 8 + amm_config 32 + pool_creator 32 → mint0 @72
    return parse_amm2_pair(data, 72, 104, 168, 200)


def parse_damm(data: bytes) -> dict | None:
    """DAMM v2 Pool: token_a/b mint then vaults. SOL mint locates the pair."""
    if len(data) < 200:
        return None
    sol = d.b58decode(SOL)
    off = find_mint(data, sol)
    if off >= 8:
        if off + 32 <= len(data) and off + 32 + 32 <= len(data):
            nxt = data[off + 32:off + 64]
            if nxt != sol:
                return {
                    "mint_x": data[off:off + 32],
                    "mint_y": nxt,
                    "vault_x": data[off + 64:off + 96] if off + 96 <= len(data) else bytes(32),
                    "vault_y": data[off + 96:off + 128] if off + 128 <= len(data) else bytes(32),
                    "fee_bps": 25,
                }
        if off >= 40:
            prev = data[off - 32:off]
            return {
                "mint_x": prev,
                "mint_y": sol,
                "vault_x": data[off + 32:off + 64] if off + 64 <= len(data) else bytes(32),
                "vault_y": data[off + 64:off + 96] if off + 96 <= len(data) else bytes(32),
                "fee_bps": 25,
            }
    if len(data) < 400:
        return None
    p = parse_amm2_pair(data, 168, 200, 232, 264)
    if p:
        p["fee_bps"] = 25
    return p


def looks_like_pool(kind: str, data: bytes, p: dict) -> bool:
    if p is None:
        return False
    mx, my = d._pk(p["mint_x"]), d._pk(p["mint_y"])
    if mx == my:
        return False
    bad = {TOKENKEG, TOKENZ, SYSTEM} | WANT
    if mx in bad or my in bad:
        return False
    n = len(data)
    if kind == "clmm" and n < 800:
        return False
    if kind == "orca" and (n < 600 or n > 1200):
        return False
    if kind == "cpmm" and n < 280:
        return False
    if kind == "damm" and n < 200:
        return False
    return True


def tick_start(tick: int, spacing: int, n: int) -> int:
    """Floor toward -inf. Python // matches on-chain floor_div for negatives."""
    span = max(1, spacing) * n
    return (tick // span) * span


def orca_tick_pda(pool: str, start: int) -> str:
    return d._pk(d.find_pda(
        [b"tick_array", d.b58decode(pool), struct.pack("<i", start)],
        d.b58decode(ORCA),
    ))


def clmm_tick_pda(pool: str, start: int) -> str:
    return d._pk(d.find_pda(
        [b"tick_array", d.b58decode(pool), struct.pack("<i", start)],
        d.b58decode(CLMM),
    ))


def has_tick_array(kind: str, pool: str, tick: int, spacing: int) -> bool:
    """Populate current ± neighbor required arrays. Any hit is enough to quote."""
    n = ORCA_TICK_ARRAY if kind == "orca" else CLMM_TICK_ARRAY
    start = tick_start(tick, spacing, n)
    span = max(1, spacing) * n
    starts = [start - span, start, start + span]
    pks = []
    for s in starts:
        pks.append(orca_tick_pda(pool, s) if kind == "orca" else clmm_tick_pda(pool, s))
    accs = d.get_multiple(pks)
    return any(a is not None for a in accs)


def fetch_row(pid: str, pk: str, fees, tick_for: set | None = None) -> dict | None:
    proto = PROTO[pid]
    if proto == 1:
        row = live.fetch_dlmm(pk)
        if row is None:
            return None
        row["proto"] = proto
        row["kind"] = "dlmm"
        return row
    if proto == 2:
        row = live.fetch_pump(pk, fees)
        if row is None:
            return None
        row["proto"] = proto
        row["kind"] = "pump"
        return row

    accs = d.get_multiple([pk])
    a = accs[0] if accs else None
    if a is None or a.get("owner") != pid:
        return None
    data = a["data"]
    if proto == 3:
        p = parse_clmm(data)
        kind = "clmm"
    elif proto == 4:
        p = parse_cpmm(data)
        kind = "cpmm"
    elif proto == 5:
        p = parse_damm(data)
        kind = "damm"
    else:
        p = parse_orca(data)
        kind = "orca"
    if p is None or not looks_like_pool(kind, data, p):
        return None
    mx = d._pk(p["mint_x"])
    my = d._pk(p["mint_y"])
    vx = d._pk(p["vault_x"])
    vy = d._pk(p["vault_y"])
    rx = ry = 0
    if proto in (4, 5):
        vaults = d.get_multiple([vx, vy])
        rx = d.token_amount(vaults[0]["data"]) if vaults[0] else 0
        ry = d.token_amount(vaults[1]["data"]) if vaults[1] else 0
        if rx == 0 or ry == 0:
            return None
    ticks = 0
    if proto in (3, 6):
        if tick_for and pk in tick_for:
            ticks = 1
        elif has_tick_array("orca" if proto == 6 else "clmm", pk,
                            int(p["tick"]), int(p["tick_spacing"])):
            ticks = 1
    return {
        "proto": proto,
        "kind": kind,
        "pubkey": d.b58decode(pk),
        "mint_x": p["mint_x"],
        "mint_y": p["mint_y"],
        "vault_x": p["vault_x"],
        "vault_y": p["vault_y"],
        "reserve_x": rx,
        "reserve_y": ry,
        "fee_bps": int(p.get("fee_bps") or 25),
        "status": 1,
        "tick": int(p.get("tick") or 0),
        "sqrt_price_x64": int(p.get("sqrt_price_x64") or 0),
        "liquidity": int(p.get("liquidity") or 0),
        "has_ticks": ticks,
        "slot": a["slot"],
        "sol_side": mx == SOL or my == SOL,
        "token": (mx if my == SOL else my) if (mx == SOL or my == SOL) else mx,
    }


def write_amm2(f, r: dict) -> None:
    live.write_u64(f, r["reserve_x"])
    live.write_u64(f, r["reserve_y"])
    live.write_u16(f, r["fee_bps"])
    live.write_u8(f, r["status"])


def write_clmm(f, r: dict) -> None:
    live.write_i32(f, r["tick"])
    live.write_u64(f, r["sqrt_price_x64"])
    live.write_u64(f, r["liquidity"])
    live.write_u16(f, r["fee_bps"])
    live.write_u8(f, r["status"])
    live.write_u8(f, r["has_ticks"])


def main() -> int:
    live.load_dotenv()
    ap = argparse.ArgumentParser()
    ap.add_argument("--arbs", default=str(Path(__file__).resolve().parent / "all_arbs_trial.jsonl"))
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "live002" / "liveuniv.bin"))
    ap.add_argument("--top-tx", type=int, default=800)
    ap.add_argument("--candidates", default="")
    args = ap.parse_args()

    print("LIVE-002  seed from 171k our-six arbs", flush=True)
    cand_path = Path(args.candidates) if args.candidates else Path(args.out).parent / "candidates.json"
    found: dict[str, str] = {}
    score: dict[str, float] = defaultdict(float)

    if cand_path.exists():
        blob = json.loads(cand_path.read_text(encoding="utf-8"))
        for pk, v in blob.items():
            found[pk] = v["pid"]
            score[pk] = float(v.get("usd") or 0)
        print(f"  loaded candidates={len(found)} from {cand_path}", flush=True)
    else:
        seeds = load_seeds(Path(args.arbs), args.top_tx)
        print(f"  seed txs={len(seeds)}", flush=True)
        for i, r in enumerate(seeds):
            gp = float(r.get("pure_profit") or 0)
            try:
                tx = d.rpc("getTransaction", [
                    r["signature"],
                    {"encoding": "json", "maxSupportedTransactionVersion": 1},
                ])
            except Exception as e:
                print(f"  tx skip {type(e).__name__}", flush=True)
                continue
            for pid, pk in ix_accounts(tx):
                if pk not in found:
                    found[pk] = pid
                score[pk] += gp
            if (i + 1) % 50 == 0:
                print(f"  scanned {i+1}/{len(seeds)} unique_accs={len(found)}", flush=True)
        cand_path.parent.mkdir(parents=True, exist_ok=True)
        cand_path.write_text(json.dumps(
            {pk: {"pid": pid, "usd": score[pk]} for pk, pid in found.items()},
            indent=2,
        ) + "\n", encoding="utf-8")
        print(f"  wrote {cand_path} unique_accs={len(found)}", flush=True)

    gcfg_pk = d._pk(d.find_pda([b"global_config"], d.b58decode(PUMP)))
    gacc = d.get_multiple([gcfg_pk])[0]
    fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)

    keys = list(found)
    print(f"  owner-verify {len(keys)} accounts", flush=True)
    accs = d.get_multiple(keys)
    owned: list[tuple[str, str, float]] = []
    own_n = defaultdict(int)
    tick_for: set[str] = set()
    for pk, acc in zip(keys, accs):
        pid = found[pk]
        if acc is None or acc.get("owner") != pid:
            continue
        data = acc["data"]
        if pid == ORCA and len(data) > 2000 and len(data) >= 44:
            tick_for.add(d._pk(data[12:44]))
        if pid == CLMM and len(data) > 2000 and len(data) >= 40:
            tick_for.add(d._pk(data[8:40]))
        owned.append((pk, pid, score[pk]))
        own_n[PROTO[pid]] += 1
    print(f"  owner-ok={len(owned)} by proto {dict(own_n)} tick_arrays_for={len(tick_for)}", flush=True)
    owned.sort(key=lambda x: -x[2])
    kind_of = {1: "dlmm", 2: "pump", 3: "clmm", 4: "cpmm", 5: "damm", 6: "orca"}
    by_p: dict[int, list] = defaultdict(list)
    for item in owned:
        by_p[PROTO[item[1]]].append(item)
    shortlist: list[tuple[str, str, float]] = []
    for proto, items in by_p.items():
        kind = kind_of[proto]
        items.sort(key=lambda x: -x[2])
        take = items[: QUOTA[kind] + 20]
        shortlist.extend(take)
        print(f"  shortlist {kind} {len(take)}/{len(items)}", flush=True)
    shortlist.sort(key=lambda x: -x[2])

    fetched: list[dict] = []
    for pk, pid, usd in shortlist:
        try:
            row = fetch_row(pid, pk, fees, tick_for)
        except Exception as e:
            print(f"  fetch fail {pk[:8]} {type(e).__name__}", flush=True)
            continue
        if row is None:
            print(f"  skip {PROTO[pid]} {pk[:8]}", flush=True)
            continue
        row["pubkey_s"] = pk
        row["usd"] = usd
        fetched.append(row)
        print(
            f"  ok {row['kind']:5} {pk[:8]}.. sol={int(row['sol_side'])} "
            f"${usd:,.0f} ticks={row.get('has_ticks', 0)}",
            flush=True,
        )

    by = defaultdict(list)
    for r in fetched:
        by[r["kind"]].append(r)
    rows = []
    used = set()
    for kind, q in QUOTA.items():
        xs = sorted(by[kind], key=lambda r: -r["usd"])[:q]
        for r in xs:
            rows.append(r)
            used.add(id(r))
    leftover = sorted((r for r in fetched if id(r) not in used), key=lambda r: -r["usd"])
    for r in leftover:
        if len(rows) >= POOL_MAX:
            break
        rows.append(r)
    rows.sort(key=lambda r: -r["usd"])
    if len(rows) > POOL_MAX:
        rows = rows[:POOL_MAX]

    if not any(r["kind"] == "dlmm" for r in rows) or not any(r["kind"] == "pump" for r in rows):
        print("need at least one DLMM and one Pump", file=sys.stderr)
        return 1

    slot = max((r["slot"] for r in rows), default=0)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        live.write_u32(f, LIVE_MAGIC)
        live.write_u16(f, LIVE_VER)
        live.write_u16(f, len(rows))
        live.write_u64(f, slot)
        live.write_u64(f, slot if slot else 1)
        for r in rows:
            live.write_meta(f, r["proto"], r["pubkey"], r["mint_x"], r["mint_y"],
                            r["vault_x"], r["vault_y"])
            if r["kind"] == "dlmm":
                live.write_dlmm(f, r["snap"])
            elif r["kind"] == "pump":
                live.write_pump(f, r)
            elif r["kind"] in ("cpmm", "damm"):
                write_amm2(f, r)
            else:
                write_clmm(f, r)

    counts = defaultdict(int)
    ticks_ok = 0
    for r in rows:
        counts[r["kind"]] += 1
        if r.get("has_ticks"):
            ticks_ok += 1
    meta = {
        "path": str(out),
        "n": len(rows),
        "counts": dict(counts),
        "tick_arrays": ticks_ok,
        "slot": slot,
        "ver": LIVE_VER,
        "pools": [
            {
                "idx": i,
                "kind": r["kind"],
                "pubkey": r["pubkey_s"],
                "sol_side": r["sol_side"],
                "usd": r["usd"],
                "has_ticks": r.get("has_ticks", 0),
            }
            for i, r in enumerate(rows)
        ],
    }
    (out.parent / "liveuniv.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out} n={len(rows)} counts={dict(counts)} slot={slot}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
