#!/usr/bin/env python3
"""Ugly CORE-004 recorder. Lives outside arb-core. Uses RPC. Allocates. Fine.

Watches live DLMM pools. When exactly one successful swap2 lands between two
account snapshots, emit a JSONL triple. json_to_cap packs that into .cap.

Env: HELIUS_RPC_URL or HELIUS_API_KEY
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
from collections import defaultdict
from pathlib import Path

_P = 2**255 - 19
_D = (-121665 * pow(121666, _P - 2, _P)) % _P

_RPC_MIN_INTERVAL = 0.2
_rpc_next = 0.0


class RateLimit(Exception):
    pass

DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
TOKEN_2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
SWAP2 = bytes([65, 75, 63, 76, 235, 91, 91, 136])
SWAP1 = bytes([248, 198, 158, 145, 225, 117, 135, 200])
LBPAIR_DISC = bytes([33, 11, 49, 98, 181, 101, 177, 13])
BINARR_DISC = bytes([92, 142, 92, 220, 5, 148, 70, 181])  # may be verified live
MAX_BIN = 70
BIN_SIZE = 144
TOKEN_PROGRAMS = {
    "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
    TOKEN_2022,
}


def rpc_url() -> str:
    u = os.environ.get("HELIUS_RPC_URL")
    if u:
        return u
    k = os.environ.get("HELIUS_API_KEY")
    if not k:
        raise SystemExit("set HELIUS_API_KEY or HELIUS_RPC_URL")
    return f"https://mainnet.helius-rpc.com/?api-key={k}"


def _pace() -> None:
    global _rpc_next
    now = time.time()
    if now < _rpc_next:
        time.sleep(_rpc_next - now)
    _rpc_next = time.time() + _RPC_MIN_INTERVAL


def rpc(method: str, params, retries: int = 12, backoff: float = 15.0):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    last = None
    for attempt in range(max(1, retries)):
        _pace()
        req = urllib.request.Request(
            rpc_url(), data=body, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=45) as r:
                obj = json.loads(r.read().decode())
            if "error" in obj:
                err = obj["error"]
                msg = str(err)
                if "429" in msg or "rate" in msg.lower() or err.get("code") in (-32429, 429):
                    raise RateLimit(msg)
                raise RuntimeError(err)
            return obj["result"]
        except (urllib.error.HTTPError, RateLimit) as e:
            last = e
            wait = min(8.0, backoff * (attempt + 1))
            code = getattr(e, "code", 429)
            if code == 429 or isinstance(e, RateLimit):
                hdrs = getattr(e, "headers", None)
                ra = hdrs.get("Retry-After") if hdrs else None
                if ra:
                    try:
                        wait = min(8.0, max(wait, float(ra)))
                    except ValueError:
                        pass
                print(f"rpc 429 {method} backoff {wait:.0f}s", flush=True)
            else:
                print(f"rpc http {code} {method} retry {attempt}", flush=True)
            time.sleep(wait)
        except Exception as e:
            last = e
            time.sleep(min(8.0, 1.0 * (attempt + 1)))
    raise last


def b58decode(s: str) -> bytes:
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
    raw = b"\x00" * pad + h
    return raw[-32:] if len(raw) >= 32 else raw.rjust(32, b"\x00")


def get_multiple(keys: list[str], retries: int = 12) -> list[dict | None]:
    out: list[dict | None] = []
    for i in range(0, len(keys), 100):
        chunk = keys[i : i + 100]
        res = rpc("getMultipleAccounts", [chunk, {"encoding": "base64"}], retries=retries, backoff=2.0)
        ctx_slot = res["context"]["slot"]
        for acc in res["value"]:
            if acc is None:
                out.append(None)
                continue
            raw = __import__("base64").b64decode(acc["data"][0])
            out.append(
                {
                    "slot": ctx_slot,
                    "owner": acc["owner"],
                    "data": raw,
                    "lamports": acc["lamports"],
                    "executable": acc.get("executable"),
                }
            )
    return out


def parse_lbpair(data: bytes) -> dict:
    if len(data) < 904 or data[:8] != LBPAIR_DISC:
        raise ValueError("not LbPair")
    off = 8
    base_factor, filter_period, decay_period, reduction_factor = struct.unpack_from("<HHHH", data, off)
    off += 8
    variable_fee_control, max_volatility_accumulator = struct.unpack_from("<II", data, off)
    off += 8
    min_bin_id, max_bin_id = struct.unpack_from("<ii", data, off)
    off += 8
    protocol_share, base_fee_power_factor, function_type, collect_fee_mode = struct.unpack_from(
        "<HBBB", data, off
    )
    off += 8  # HBBB + 3 pad
    vol_acc, vol_ref, idx_ref = struct.unpack_from("<IIi", data, off)
    off += 16  # 12 + 4 pad
    last_upd = struct.unpack_from("<q", data, off)[0]
    off += 16  # i64 + 8 pad
    # bump 1, bin_step_seed 2, pair_type 1
    off += 4
    active_id, bin_step, status = struct.unpack_from("<iHB", data, off)
    off += 7
    # require_base_factor_seed 1, base_factor_seed 2, activation_type 1, creator 1
    off += 5
    token_x = data[off : off + 32]
    token_y = data[off + 32 : off + 64]
    reserve_x = data[off + 64 : off + 96]
    reserve_y = data[off + 96 : off + 128]
    return {
        "base_factor": base_factor,
        "filter_period": filter_period,
        "decay_period": decay_period,
        "reduction_factor": reduction_factor,
        "variable_fee_control": variable_fee_control,
        "max_volatility_accumulator": max_volatility_accumulator,
        "protocol_share": protocol_share,
        "base_fee_power_factor": base_fee_power_factor,
        "collect_fee_mode": collect_fee_mode,
        "vol_acc": vol_acc,
        "vol_ref": vol_ref,
        "idx_ref": idx_ref,
        "last_upd": last_upd,
        "active_id": active_id,
        "bin_step": bin_step,
        "status": status,
        "token_x": token_x,
        "token_y": token_y,
        "vault_x": reserve_x,
        "vault_y": reserve_y,
    }


def parse_bin_array(data: bytes) -> list[dict]:
    # try with and without verifying disc; size is the check
    if len(data) < 10136:
        return []
    body = data[8:] if len(data) >= 10136 else data
    index = struct.unpack_from("<q", body, 0)[0]
    # version u8 + pad 7 + pubkey 32 → bins at 48
    bins_off = 48
    lo = int(index) * MAX_BIN
    out = []
    for i in range(MAX_BIN):
        o = bins_off + i * BIN_SIZE
        ax, ay = struct.unpack_from("<QQ", body, o)
        out.append({"id": lo + i, "x": ax, "y": ay})
    return out


def token_amount(data: bytes) -> int:
    if len(data) < 72:
        return 0
    return struct.unpack_from("<Q", data, 64)[0]


def tx_keys(tx: dict) -> list[str]:
    msg = tx["transaction"]["message"]
    keys = list(msg.get("accountKeys") or [])
    loaded = (tx.get("meta") or {}).get("loadedAddresses") or {}
    keys.extend(loaded.get("writable") or [])
    keys.extend(loaded.get("readonly") or [])
    # json encoding may already be strings
    norm = []
    for k in keys:
        if isinstance(k, dict):
            norm.append(k["pubkey"])
        else:
            norm.append(k)
    return norm


def decode_swaps(tx: dict) -> list[dict]:
    meta = tx.get("meta") or {}
    if meta.get("err"):
        return []
    msg = tx["transaction"]["message"]
    keys = tx_keys(tx)
    ixs = msg.get("instructions") or []
    inner = []
    for group in meta.get("innerInstructions") or []:
        inner.extend(group.get("instructions") or [])
    out = []
    for ix in list(ixs) + inner:
        pid = ix.get("programId")
        if pid is None:
            idx = ix.get("programIdIndex")
            pid = keys[idx] if idx is not None and idx < len(keys) else None
        if pid != DLMM:
            continue
        raw = ix.get("data")
        if isinstance(raw, str):
            data = _b58_any(raw)
        else:
            data = bytes(raw or [])
        if len(data) < 24 or data[:8] not in (SWAP2, SWAP1):
            continue
        amount_in, min_out = struct.unpack_from("<QQ", data, 8)
        accs = []
        for a in ix.get("accounts") or []:
            if isinstance(a, int):
                accs.append(keys[a])
            else:
                accs.append(a)
        if not accs:
            continue
        out.append(
            {
                "pair": accs[0],
                "amount_in": amount_in,
                "min_out": min_out,
                "accounts": accs,
                "keys": keys,
            }
        )
    return out


def decode_swap2(tx: dict) -> dict | None:
    sw = decode_swaps(tx)
    return sw[0] if sw else None


def pair_events(pair: str, lo_slot: int, hi_slot: int) -> int:
    """Successful txs mentioning `pair` with lo < slot <= hi. Newest first."""
    before = None
    n = 0
    for _ in range(8):
        opts: dict = {"limit": 30}
        if before:
            opts["before"] = before
        sigs = rpc("getSignaturesForAddress", [pair, opts])
        if not sigs:
            break
        for s in sigs:
            slot = s.get("slot") or 0
            if slot <= lo_slot:
                return n
            if slot <= hi_slot and not s.get("err"):
                n += 1
                if n > 1:
                    return n
        before = sigs[-1]["signature"]
        if (sigs[-1].get("slot") or 0) <= lo_slot:
            break
    return n


def _b58_any(s: str) -> bytes:
    alph = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n = 0
    for c in s:
        n = n * 58 + alph.index(c)
    h = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    return (b"\x00" * pad + h)


def recent_pairs(want: int = 6, scan: int = 16) -> dict[str, list[str]]:
    """pair -> accounts worth watching. Scans `scan` sigs, keeps up to `want` pools."""
    sigs = rpc("getSignaturesForAddress", [DLMM, {"limit": scan}])
    pairs: dict[str, set[str]] = defaultdict(set)
    for s in sigs:
        tx = rpc(
            "getTransaction",
            [s["signature"], {"encoding": "json", "maxSupportedTransactionVersion": 1}],
        )
        if not tx:
            continue
        sw = decode_swap2(tx)
        if not sw:
            continue
        if sw["pair"] not in pairs and len(pairs) >= want:
            continue
        pairs[sw["pair"]].update(sw["accounts"])
        pairs[sw["pair"]].update(sw["keys"])
    return {k: list(v) for k, v in pairs.items()}


def load_seen(path: Path) -> set[str]:
    seen: set[str] = set()
    if not path.exists():
        return seen
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        sig = rec.get("sig")
        if sig:
            seen.add(sig)
    return seen


def count_triples(path: Path) -> int:
    if not path.exists():
        return 0
    return sum(1 for line in path.read_text(encoding="utf-8").splitlines() if line.startswith("{"))


def _on_curve(pt: bytes) -> bool:
    """Solana PDA reject: 32 bytes decompress as an Edwards point."""
    if len(pt) != 32:
        return False
    y = int.from_bytes(pt, "little") & ((1 << 255) - 1)
    if y >= _P:
        return False
    y2 = pow(y, 2, _P)
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


def bin_array_index(bin_id: int) -> int:
    # Meteora: floor-div (negative remainder pulls index down by 1).
    return bin_id // MAX_BIN


def bin_array_pda(pair: str, index: int) -> str:
    raw = find_pda(
        [b"bin_array", b58decode(pair), struct.pack("<q", index)],
        b58decode(DLMM),
    )
    return _pk(raw)


def array_indexes(active_id: int) -> list[int]:
    lo = bin_array_index(active_id - 16)
    hi = bin_array_index(active_id + 16)
    return list(range(lo, hi + 1))


def array_indexes_k(active_id: int, k: int) -> list[int]:
    lo = bin_array_index(active_id - k)
    hi = bin_array_index(active_id + k)
    return list(range(lo, hi + 1))


def pricing_snap(pair: str, k: int = 64) -> dict | None:
    """LbPair + real BinArrays. reserve_* is bin-sum, never vault."""
    accs = get_multiple([pair])
    if not accs or not accs[0] or accs[0]["owner"] != DLMM:
        return None
    try:
        lb = parse_lbpair(accs[0]["data"])
    except ValueError:
        return None
    keys = [bin_array_pda(pair, i) for i in array_indexes_k(lb["active_id"], k)]
    arrs = get_multiple(keys)
    bins = []
    for acc in arrs:
        if acc:
            bins.extend(parse_bin_array(acc["data"]))
    use = [b for b in bins if abs(b["id"] - lb["active_id"]) <= k]
    use.sort(key=lambda b: b["id"])
    vx = _pk(lb["vault_x"])
    vy = _pk(lb["vault_y"])
    vaults = get_multiple([vx, vy])
    vault_x = token_amount(vaults[0]["data"]) if vaults and vaults[0] else None
    vault_y = token_amount(vaults[1]["data"]) if vaults and vaults[1] else None
    return {
        "slot": accs[0]["slot"],
        "pair": pair,
        "lb": lb,
        "bins": use,
        "reserve_x": sum(b["x"] for b in use),
        "reserve_y": sum(b["y"] for b in use),
        "vault_x": vault_x,
        "vault_y": vault_y,
        "k": k,
    }


def pool_keys(pair: str, lb: dict) -> list[str]:
    keys = [pair, _pk(lb["vault_x"]), _pk(lb["vault_y"])]
    for idx in array_indexes(lb["active_id"]):
        keys.append(bin_array_pda(pair, idx))
    return list(dict.fromkeys(k for k in keys if k))


def watch_keys(pair: str, candidates: list[str], prev: dict | None) -> list[str]:
    if prev and prev.get("lb"):
        return pool_keys(pair, prev["lb"])
    return [pair]


def snap_from_accounts(pair: str, by: dict) -> dict | None:
    pair_acc = by.get(pair)
    if not pair_acc or pair_acc["owner"] != DLMM:
        return None
    try:
        lb = parse_lbpair(pair_acc["data"])
    except ValueError:
        return None
    bins = []
    for k, a in by.items():
        if a["owner"] != DLMM or len(a["data"]) < 10136:
            continue
        if _pk(a["data"][24:56]) != pair:
            continue
        bins.extend(parse_bin_array(a["data"]))
    vx = by.get(_pk(lb["vault_x"]))
    vy = by.get(_pk(lb["vault_y"]))
    reserve_x = token_amount(vx["data"]) if vx else sum(b["x"] for b in bins)
    reserve_y = token_amount(vy["data"]) if vy else sum(b["y"] for b in bins)
    watch = [pair, _pk(lb["vault_x"]), _pk(lb["vault_y"])]
    for k, a in by.items():
        if a["owner"] == DLMM and (len(a["data"]) >= 10136 or k == pair):
            watch.append(k)
    return {
        "slot": pair_acc["slot"],
        "lb": lb,
        "bins": bins,
        "reserve_x": reserve_x,
        "reserve_y": reserve_y,
        "watch": list(dict.fromkeys(watch))[:10],
    }


def _merge_missing(pair: str, by: dict) -> dict | None:
    sn = snap_from_accounts(pair, by)
    if sn is None:
        return None
    miss = [k for k in pool_keys(pair, sn["lb"]) if k not in by]
    if miss:
        accs = get_multiple(miss)
        by.update({k: a for k, a in zip(miss, accs) if a})
        sn = snap_from_accounts(pair, by) or sn
    return sn


def snapshot_pool(pair: str, candidates: list[str], prev: dict | None = None) -> dict | None:
    ukeys = watch_keys(pair, candidates, prev)
    accs = get_multiple(ukeys)
    by = {k: a for k, a in zip(ukeys, accs) if a}
    return _merge_missing(pair, by)


def refresh_all(pairs: dict, snaps: dict) -> None:
    keys: list[str] = []
    seen: set[str] = set()
    for pair, cands in pairs.items():
        for k in watch_keys(pair, cands, snaps.get(pair)):
            if k not in seen:
                seen.add(k)
                keys.append(k)
    if not keys:
        return
    accs = get_multiple(keys)
    by = {k: a for k, a in zip(keys, accs) if a}
    extra: list[str] = []
    extra_seen: set[str] = set()
    for pair in pairs:
        sn = snap_from_accounts(pair, by)
        if sn is None:
            continue
        for k in pool_keys(pair, sn["lb"]):
            if k not in by and k not in extra_seen:
                extra_seen.add(k)
                extra.append(k)
    if extra:
        accs2 = get_multiple(extra)
        by.update({k: a for k, a in zip(extra, accs2) if a})
    for pair in pairs:
        sn = snap_from_accounts(pair, by)
        if sn:
            snaps[pair] = sn


def _pk(raw: bytes) -> str:
    alph = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n = int.from_bytes(raw, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = alph[r] + out
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return "1" * pad + (out or "1")


def emit_triple(pool_idx: int, before: dict, after: dict, sw: dict, block_time: int) -> dict:
    lb0, lb1 = before["lb"], after["lb"]
    mint_x = lb0["token_x"]
    # swap_for_y if the user spent X (input mint is X). Infer from token balances if possible.
    # Default: compare user source against mint — we only have amount; use vault delta.
    swap_for_y = 1 if after["reserve_x"] > before["reserve_x"] else 0
    window = [b for b in before["bins"] if abs(b["id"] - lb0["active_id"]) <= 16]
    window_after = [b for b in after["bins"] if abs(b["id"] - lb0["active_id"]) <= 16]
    if not window_after:
        window_after = [b for b in after["bins"] if abs(b["id"] - lb1["active_id"]) <= 16]
    return {
        "pool_idx": pool_idx,
        "swap_for_y": swap_for_y,
        "amount_in": sw["amount_in"],
        "min_out": sw["min_out"],
        "now_ts": block_time or int(time.time()),
        "before_active": lb0["active_id"],
        "after_active": lb1["active_id"],
        "bin_step": lb0["bin_step"],
        "status": lb0["status"],
        "base_factor": lb0["base_factor"],
        "filter_period": lb0["filter_period"],
        "decay_period": lb0["decay_period"],
        "reduction_factor": lb0["reduction_factor"],
        "variable_fee_control": lb0["variable_fee_control"],
        "max_volatility_accumulator": lb0["max_volatility_accumulator"],
        "protocol_share": lb0["protocol_share"],
        "base_fee_power_factor": lb0["base_fee_power_factor"],
        "collect_fee_mode": lb0["collect_fee_mode"],
        "vol_acc_before": lb0["vol_acc"],
        "vol_ref_before": lb0["vol_ref"],
        "idx_ref_before": lb0["idx_ref"],
        "last_upd_before": lb0["last_upd"],
        "vol_acc_after": lb1["vol_acc"],
        "vol_ref_after": lb1["vol_ref"],
        "idx_ref_after": lb1["idx_ref"],
        "last_upd_after": lb1["last_upd"],
        "before_rx": sum(b["x"] for b in window),
        "before_ry": sum(b["y"] for b in window),
        "after_rx": sum(b["x"] for b in window_after),
        "after_ry": sum(b["y"] for b in window_after),
        "vault_rx_before": before["reserve_x"],
        "vault_ry_before": before["reserve_y"],
        "vault_rx_after": after["reserve_x"],
        "vault_ry_after": after["reserve_y"],
        "bins_before": window,
        "bins_after": window_after,
        "before_slot": before["slot"],
        "after_slot": after["slot"],
        "pair": sw["pair"],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", type=int, default=80)
    ap.add_argument("--out", default=str(Path(__file__).with_name("triples.jsonl")))
    ap.add_argument("--watch", type=int, default=10)
    args = ap.parse_args()
    out = Path(args.out)
    seen = load_seen(out)
    have = count_triples(out)
    print(f"discovering pools (have {have}, target {args.target})...", flush=True)
    pairs = recent_pairs(args.watch, scan=40)
    print(f"watching {len(pairs)} pools, seen {len(seen)} sigs", flush=True)
    if not pairs:
        return 1
    last_prog_sig = None
    snaps: dict[str, dict] = {}
    idx = {p: i for i, p in enumerate(pairs)}
    for pair, cands in pairs.items():
        sn = snapshot_pool(pair, cands)
        if sn:
            snaps[pair] = sn
            print(f"  snap {pair[:8]} slot {sn['slot']} bins {len(sn['bins'])}", flush=True)

    with out.open("a", encoding="utf-8") as f:
        while have < args.target:
            try:
                sigs = rpc("getSignaturesForAddress", [DLMM, {"limit": 15}])
            except Exception as e:
                print("sig err", e, flush=True)
                time.sleep(5)
                continue
            new_heads = []
            for s in sigs:
                if s["signature"] == last_prog_sig:
                    break
                if s.get("err"):
                    continue
                if s["signature"] in seen:
                    continue
                new_heads.append(s)
            if sigs:
                last_prog_sig = sigs[0]["signature"]

            by_pair: dict[str, list] = defaultdict(list)
            for s in reversed(new_heads):
                try:
                    tx = rpc(
                        "getTransaction",
                        [
                            s["signature"],
                            {"encoding": "json", "maxSupportedTransactionVersion": 1},
                        ],
                    )
                except Exception as e:
                    print("tx err", e, flush=True)
                    continue
                if not tx:
                    continue
                swaps = decode_swaps(tx)
                if not swaps:
                    continue
                for sw in swaps:
                    if sw["pair"] not in snaps:
                        if len(snaps) < 12:
                            cands = list(dict.fromkeys(sw["accounts"] + sw["keys"]))
                            sn = snapshot_pool(sw["pair"], cands)
                            if sn:
                                snaps[sw["pair"]] = sn
                                pairs[sw["pair"]] = cands
                                idx[sw["pair"]] = len(idx)
                                print(f"  adopt {sw['pair'][:8]} slot {sn['slot']}", flush=True)
                        continue
                    by_pair[sw["pair"]].append((s, tx, sw))

            for pair, decoded in by_pair.items():
                if have >= args.target:
                    break
                if len(decoded) != 1:
                    snaps[pair] = snapshot_pool(pair, pairs[pair]) or snaps[pair]
                    for s, _, _ in decoded:
                        seen.add(s["signature"])
                    continue
                s, tx, sw = decoded[0]
                before = snaps.get(pair)
                tx_slot = tx.get("slot") or 0
                if before is None or before["slot"] >= tx_slot:
                    snaps[pair] = snapshot_pool(pair, pairs[pair]) or before
                    seen.add(s["signature"])
                    continue
                after = snapshot_pool(pair, pairs[pair])
                if after is None:
                    continue
                gap = after["slot"] - before["slot"]
                if gap > 16:
                    print(f"drop alignment gap={gap} {pair[:8]}", flush=True)
                    snaps[pair] = after
                    seen.add(s["signature"])
                    continue
                try:
                    nev = pair_events(pair, before["slot"], after["slot"])
                except Exception as e:
                    print("pair sig err", e, flush=True)
                    snaps[pair] = after
                    seen.add(s["signature"])
                    continue
                if nev != 1:
                    print(f"drop alignment events={nev} {pair[:8]} gap={gap}", flush=True)
                    snaps[pair] = after
                    seen.add(s["signature"])
                    continue
                rec = emit_triple(idx[pair], before, after, sw, tx.get("blockTime") or 0)
                if not rec["bins_before"] or not rec["bins_after"]:
                    print(
                        f"drop coverage bins={len(rec['bins_before'])}/{len(rec['bins_after'])} "
                        f"{pair[:8]}",
                        flush=True,
                    )
                    snaps[pair] = after
                    seen.add(s["signature"])
                    continue
                rec["sig"] = s["signature"]
                rec["tx_slot"] = tx_slot
                rec["slot_gap"] = gap
                f.write(json.dumps(rec) + "\n")
                f.flush()
                seen.add(s["signature"])
                have += 1
                snaps[pair] = after
                print(
                    f"triple {have}/{args.target} {pair[:8]} "
                    f"walk={abs(rec['after_active'] - rec['before_active'])} "
                    f"gap={gap} slots {before['slot']}->{after['slot']}",
                    flush=True,
                )
            try:
                refresh_all(pairs, snaps)
            except Exception as e:
                print("refresh err", e, flush=True)
            time.sleep(0.8)

    print(f"done {have} triples -> {out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
