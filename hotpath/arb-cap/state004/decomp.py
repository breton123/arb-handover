#!/usr/bin/env python3
"""STATE-004 — vault / fee / event decomposition for landed corpus34.

RPC observation only. Never prints secrets. No send.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
# Anchor emit_cpi! envelope (same for every Anchor program).
ANCHOR_EVENT_IX = bytes.fromhex("e445a52e51cb9a1d")
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def event_disc(name: str) -> bytes:
    return hashlib.sha256(f"event:{name}".encode()).digest()[:8]


DISC_SWAP = event_disc("Swap")
DISC_SWAP2 = event_disc("Swap2Evt")


def b58decode_data(s: str) -> bytes:
    n = 0
    for ch in s:
        n = n * 58 + B58.index(ch)
    raw = n.to_bytes((n.bit_length() + 7) // 8 or 1, "big")
    pad = 0
    for ch in s:
        if ch == "1":
            pad += 1
        else:
            break
    return b"\x00" * pad + raw


def rpc_url() -> str:
    u = (os.environ.get("HELIUS_RPC_URL") or os.environ.get("RPC_URL") or "").strip()
    if u:
        return u
    k = (os.environ.get("HELIUS_API_KEY") or "").strip()
    if not k:
        raise SystemExit("set HELIUS_API_KEY")
    return f"https://mainnet.helius-rpc.com/?api-key={k}"


def rpc(method: str, params, pause=0.08):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    last = None
    for _ in range(8):
        time.sleep(pause)
        req = urllib.request.Request(
            rpc_url(), data=body, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=45) as r:
                obj = json.loads(r.read().decode())
            if "error" in obj:
                msg = str(obj["error"])
                if "429" in msg or "rate" in msg.lower():
                    time.sleep(1.2)
                    last = msg
                    continue
                raise RuntimeError(msg)
            return obj.get("result")
        except urllib.error.HTTPError as e:
            last = str(e)
            if e.code == 429:
                time.sleep(1.2)
                continue
            raise
        except Exception as e:
            last = str(e)
            time.sleep(0.4)
    raise RuntimeError(last or "rpc")


def tx_keys(tx: dict) -> list[str]:
    msg = ((tx.get("transaction") or {}).get("message") or {})
    keys = []
    for k in msg.get("accountKeys") or []:
        keys.append(k.get("pubkey") if isinstance(k, dict) else str(k))
    loaded = (tx.get("meta") or {}).get("loadedAddresses") or {}
    keys.extend(loaded.get("writable") or [])
    keys.extend(loaded.get("readonly") or [])
    return keys


def ui_amt(b: dict) -> int | None:
    ui = (b.get("uiTokenAmount") or {}).get("amount")
    return int(ui) if ui is not None else None


def bals(tx: dict, which: str) -> dict[str, int]:
    meta = tx.get("meta") or {}
    keys = tx_keys(tx)
    out: dict[str, int] = {}
    for b in meta.get(which) or []:
        idx = b.get("accountIndex")
        pk = keys[idx] if idx is not None and idx < len(keys) else ""
        amt = ui_amt(b)
        if pk and amt is not None:
            out[pk] = amt
    return out


def mint_of(tx: dict, acct: str) -> str | None:
    meta = tx.get("meta") or {}
    keys = tx_keys(tx)
    for b in (meta.get("preTokenBalances") or []) + (meta.get("postTokenBalances") or []):
        idx = b.get("accountIndex")
        pk = keys[idx] if idx is not None and idx < len(keys) else ""
        if pk == acct:
            return b.get("mint")
    return None


def host_fee_pda(pair: str) -> str:
    return d._pk(d.find_pda([b"host_fee", d.b58decode(pair)], d.b58decode(DLMM)))


def parse_swap_body(body: bytes) -> dict | None:
    # Swap: pk pk i32 i32 u64 u64 bool u64 u64 u128 u64
    need = 32 + 32 + 4 + 4 + 8 + 8 + 1 + 8 + 8 + 16 + 8
    if len(body) < need:
        return None
    o = 0
    lb = body[o : o + 32]
    o += 32
    user = body[o : o + 32]
    o += 32
    start, end = struct.unpack_from("<ii", body, o)
    o += 8
    ain, aout = struct.unpack_from("<QQ", body, o)
    o += 16
    s4y = body[o]
    o += 1
    fee, proto = struct.unpack_from("<QQ", body, o)
    o += 16
    fee_bps_lo, fee_bps_hi = struct.unpack_from("<QQ", body, o)
    o += 16
    host = struct.unpack_from("<Q", body, o)[0]
    return {
        "kind": "Swap",
        "lb_pair": d._pk(lb),
        "from": d._pk(user),
        "start_bin_id": start,
        "end_bin_id": end,
        "amount_in": ain,
        "amount_out": aout,
        "swap_for_y": bool(s4y),
        "fee": fee,
        "protocol_fee": proto,
        "fee_bps": fee_bps_lo | (fee_bps_hi << 64),
        "host_fee": host,
        "mm_fee": None,
        "limit_order_fee": None,
        "fees_on_input": None,
        "fees_on_token_x": None,
    }


def parse_swap2_body(body: bytes) -> dict | None:
    # Swap2Evt: pk pk i32 i32 bool u128 u64 u64 u64 u64 u64 u64 u64 bool bool
    need = 32 + 32 + 4 + 4 + 1 + 16 + 8 * 7 + 1 + 1
    if len(body) < need:
        return None
    o = 0
    lb = body[o : o + 32]
    o += 32
    user = body[o : o + 32]
    o += 32
    start, end = struct.unpack_from("<ii", body, o)
    o += 8
    s4y = body[o]
    o += 1
    fee_bps_lo, fee_bps_hi = struct.unpack_from("<QQ", body, o)
    o += 16
    ain, left, aout, mm, proto, lo_fee, host = struct.unpack_from("<QQQQQQQ", body, o)
    o += 56
    foi = body[o]
    o += 1
    fox = body[o]
    return {
        "kind": "Swap2Evt",
        "lb_pair": d._pk(lb),
        "from": d._pk(user),
        "start_bin_id": start,
        "end_bin_id": end,
        "amount_in": ain,
        "amount_left": left,
        "amount_out": aout,
        "swap_for_y": bool(s4y),
        "fee": mm + proto + lo_fee + host,
        "mm_fee": mm,
        "protocol_fee": proto,
        "limit_order_fee": lo_fee,
        "host_fee": host,
        "fee_bps": fee_bps_lo | (fee_bps_hi << 64),
        "fees_on_input": bool(foi),
        "fees_on_token_x": bool(fox),
    }


def decode_event_blob(raw: bytes) -> dict | None:
    for blob in (raw, raw[8:] if len(raw) > 8 else b"", raw[16:] if len(raw) > 16 else b""):
        if len(blob) < 8:
            continue
        disc, body = blob[:8], blob[8:]
        if disc == DISC_SWAP:
            ev = parse_swap_body(body)
            if ev:
                return ev
        if disc == DISC_SWAP2:
            ev = parse_swap2_body(body)
            if ev:
                return ev
    if len(raw) >= 16 and raw[:8] == ANCHOR_EVENT_IX:
        disc, body = raw[8:16], raw[16:]
        if disc == DISC_SWAP:
            return parse_swap_body(body)
        if disc == DISC_SWAP2:
            return parse_swap2_body(body)
    return None


def events_from_tx(tx: dict) -> list[dict]:
    found: list[dict] = []
    for ln in (tx.get("meta") or {}).get("logMessages") or []:
        if not ln.startswith("Program data: "):
            continue
        try:
            raw = base64.b64decode(ln.split(" ", 2)[2])
        except Exception:
            continue
        ev = decode_event_blob(raw)
        if ev:
            found.append(ev)
    for group in (tx.get("meta") or {}).get("innerInstructions") or []:
        for ix in group.get("instructions") or []:
            data = ix.get("data")
            if not data or ix.get("parsed"):
                continue
            try:
                raw = b58decode_data(data) if isinstance(data, str) else bytes(data)
            except Exception:
                continue
            ev = decode_event_blob(raw)
            if ev:
                found.append(ev)
    # de-dup identical payloads
    uniq = []
    seen = set()
    for ev in found:
        key = (
            ev.get("kind"),
            ev.get("amount_in"),
            ev.get("amount_out"),
            ev.get("fee"),
            ev.get("start_bin_id"),
            ev.get("end_bin_id"),
        )
        if key in seen:
            continue
        seen.add(key)
        uniq.append(ev)
    return uniq


def token_transfers(tx: dict) -> list[dict]:
    out = []
    for group in (tx.get("meta") or {}).get("innerInstructions") or []:
        for ix in group.get("instructions") or []:
            parsed = ix.get("parsed")
            if not isinstance(parsed, dict):
                continue
            if parsed.get("type") not in ("transfer", "transferChecked"):
                continue
            info = parsed.get("info") or {}
            amt = info.get("amount")
            if amt is None:
                tok = info.get("tokenAmount") or {}
                amt = tok.get("amount")
            if amt is None:
                continue
            out.append(
                {
                    "source": info.get("source"),
                    "dest": info.get("destination"),
                    "authority": info.get("authority"),
                    "mint": info.get("mint"),
                    "amount": int(amt),
                }
            )
    return out


def role_of(pk: str, vx: str, vy: str, host: str) -> str:
    if pk == vx:
        return "vault_x"
    if pk == vy:
        return "vault_y"
    if pk == host:
        return "host_fee"
    return "other"


def decomp_one(row: dict) -> dict:
    sig = row["sig"]
    pool = row["pool"]
    tx = rpc(
        "getTransaction",
        [sig, {"encoding": "jsonParsed", "maxSupportedTransactionVersion": 1}],
    )
    if not tx:
        return {"sig": sig[:8], "error": "no tx"}
    accs = d.get_multiple([pool])
    lb = d.parse_lbpair(accs[0]["data"]) if accs and accs[0] else None
    vx = d._pk(lb["vault_x"]) if lb else ""
    vy = d._pk(lb["vault_y"]) if lb else ""
    host = host_fee_pda(pool) if pool else ""
    pre, post = bals(tx, "preTokenBalances"), bals(tx, "postTokenBalances")
    keys = set(pre) | set(post)
    changes = []
    for pk in sorted(keys):
        a0, a1 = pre.get(pk), post.get(pk)
        if a0 is None or a1 is None or a0 == a1:
            continue
        changes.append(
            {
                "acct": pk,
                "role": role_of(pk, vx, vy, host),
                "mint": mint_of(tx, pk),
                "pre": a0,
                "post": a1,
                "delta": a1 - a0,
            }
        )
    vx_d = next((c["delta"] for c in changes if c["role"] == "vault_x"), None)
    vy_d = next((c["delta"] for c in changes if c["role"] == "vault_y"), None)
    host_moves = [c for c in changes if c["role"] == "host_fee"]
    others = [c for c in changes if c["role"] == "other"]

    evs = events_from_tx(tx)
    ev = next((e for e in evs if e.get("kind") == "Swap2Evt"), None) or (
        evs[0] if evs else None
    )
    xfers = token_transfers(tx)
    labeled = []
    for t in xfers:
        src_r = role_of(t["source"] or "", vx, vy, host)
        dst_r = role_of(t["dest"] or "", vx, vy, host)
        if src_r == "other" and dst_r == "other":
            continue
        labeled.append({**t, "src_role": src_r, "dst_role": dst_r})

    ain = int(row.get("n_ain") or 0)
    direction = int(row.get("n_dir") or 0)
    ev_in = ev["amount_in"] if ev else None
    ev_out = ev["amount_out"] if ev else None
    ev_fee = ev["fee"] if ev else None
    ev_proto = ev["protocol_fee"] if ev else None
    ev_host = ev["host_fee"] if ev else None
    ev_mm = ev.get("mm_fee") if ev else None
    ev_s4y = ev["swap_for_y"] if ev else bool(direction)

    # Vault identity from token balances + event.
    eq_x = None
    eq_y = None
    if ev and vx_d is not None and ev_s4y:
        # fees_on_input X: vault keeps full user input; fee never leaves vault.
        eq_x = {
            "vault_before": pre.get(vx),
            "plus_user_in": ev_in,
            "minus_host": ev_host or 0,
            "equals_vault_after": post.get(vx),
            "predicted": (pre.get(vx) or 0) + ev_in - (ev_host or 0),
            "exact": (pre.get(vx) or 0) + ev_in - (ev_host or 0) == (post.get(vx) or 0),
            "vault_delta_eq_user_in": vx_d == ev_in,
        }
        eq_y = {
            "vault_before": pre.get(vy),
            "minus_user_out": ev_out,
            "equals_vault_after": post.get(vy),
            "predicted": (pre.get(vy) or 0) - ev_out,
            "exact": (pre.get(vy) or 0) - ev_out == (post.get(vy) or 0),
            "vault_delta_eq_neg_out": vy_d == -ev_out if vy_d is not None else None,
        }
    elif ev and vx_d is not None and not ev_s4y:
        eq_y = {
            "vault_before": pre.get(vy),
            "plus_user_in": ev_in,
            "minus_host": ev_host or 0,
            "equals_vault_after": post.get(vy),
            "predicted": (pre.get(vy) or 0) + ev_in - (ev_host or 0),
            "exact": (pre.get(vy) or 0) + ev_in - (ev_host or 0) == (post.get(vy) or 0),
            "vault_delta_eq_user_in": vy_d == ev_in,
        }
        eq_x = {
            "vault_before": pre.get(vx),
            "minus_user_out": ev_out,
            "equals_vault_after": post.get(vx),
            "predicted": (pre.get(vx) or 0) - ev_out,
            "exact": (pre.get(vx) or 0) - ev_out == (post.get(vx) or 0),
            "vault_delta_eq_neg_out": vx_d == -ev_out,
        }

    priced_from_vault = None
    if ev and vx_d is not None and ev_s4y and ev_fee is not None:
        # If snapshot seeds reserve_* from vault, apply does reserve += in - fee.
        priced_from_vault = {
            "seeded_rx_before": pre.get(vx),
            "kernel_rule_rx_after": (pre.get(vx) or 0) + ev_in - ev_fee,
            "vault_rx_after": post.get(vx),
            "vault_minus_priced": ev_fee,
            "note": "vault_after - (vault_before + in - fee) == fee; fee stays in vault, outside bins",
        }
    if ev and vy_d is not None and ev_s4y:
        if priced_from_vault is None:
            priced_from_vault = {}
        priced_from_vault["seeded_ry_before"] = pre.get(vy)
        priced_from_vault["kernel_rule_ry_after"] = (pre.get(vy) or 0) - ev_out
        priced_from_vault["vault_ry_after"] = post.get(vy)
        priced_from_vault["y_priced_eq_vault"] = (pre.get(vy) or 0) - ev_out == (post.get(vy) or 0)

    rec = {
        "i": row.get("i"),
        "named": row.get("named"),
        "sig": sig,
        "slot": tx.get("slot"),
        "pool": pool,
        "n_ain": ain,
        "n_dir": direction,
        "vault_x": vx,
        "vault_y": vy,
        "vault_x_pre": pre.get(vx),
        "vault_x_post": post.get(vx),
        "vault_y_pre": pre.get(vy),
        "vault_y_post": post.get(vy),
        "vault_x_delta": vx_d,
        "vault_y_delta": vy_d,
        "event": ev,
        "n_events": len(evs),
        "vault_x_equation": eq_x,
        "vault_y_equation": eq_y,
        "priced_vs_vault": priced_from_vault,
        "host_fee_moves": [
            {k: c[k] for k in ("acct", "mint", "delta")} for c in host_moves
        ],
        "vault_touching_transfers": labeled,
        "other_token_moves": [
            {k: c[k] for k in ("acct", "mint", "delta")} for c in others
        ],
        "n_other": len(others),
        "err": (tx.get("meta") or {}).get("err"),
    }
    return rec


def summarize(recs: list[dict]) -> dict:
    n = len(recs)
    ev_n = sum(1 for r in recs if r.get("event"))
    x_exact = sum(1 for r in recs if (r.get("vault_x_equation") or {}).get("exact"))
    y_exact = sum(1 for r in recs if (r.get("vault_y_equation") or {}).get("exact"))
    x_in = sum(
        1
        for r in recs
        if (r.get("vault_x_equation") or {}).get("vault_delta_eq_user_in")
        or (r.get("vault_y_equation") or {}).get("vault_delta_eq_user_in")
    )
    host = sum(1 for r in recs if r.get("host_fee_moves"))
    return {
        "landed": n,
        "events_decoded": ev_n,
        "vault_in_leg_exact": x_in,
        "vault_x_equation_exact": x_exact,
        "vault_y_equation_exact": y_exact,
        "host_fee_account_moved": host,
    }


def main() -> int:
    live.load_dotenv()
    corpus = Path(sys.argv[1] if len(sys.argv) > 1 else "corpus34.json")
    out = Path(
        sys.argv[2]
        if len(sys.argv) > 2
        else corpus.parent.parent / "state004" / "STATE004.json"
    )
    blob = json.loads(corpus.read_text(encoding="utf-8"))
    landed = [r for r in blob["rows"] if r.get("landed")]
    print(f"STATE-004  landed={len(landed)}", flush=True)
    recs = []
    for r in landed:
        rec = decomp_one(r)
        recs.append(rec)
        ev = rec.get("event") or {}
        xeq = rec.get("vault_x_equation") or {}
        yeq = rec.get("vault_y_equation") or {}
        print(
            f"  i={r['i']} {(r.get('named') or r['sig'][:8])} "
            f"ev={ev.get('kind')} ain={ev.get('amount_in')} fee={ev.get('fee')} "
            f"proto={ev.get('protocol_fee')} host={ev.get('host_fee')} "
            f"bins={ev.get('start_bin_id')}->{ev.get('end_bin_id')} "
            f"vx_eq={xeq.get('exact')} vy_eq={yeq.get('exact')} "
            f"vx_d={rec.get('vault_x_delta')} vy_d={rec.get('vault_y_delta')}",
            flush=True,
        )
    report = {
        "summary": summarize(recs),
        "rows": recs,
        "note": (
            "dlmm_state_t.reserve_* follows into_bin = included - fee (priced bin liquidity). "
            "SPL vaults receive the full user input; fee/protocol/LP remain in the vault "
            "until claimed. Do not force reserve_* == vault."
        ),
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}", flush=True)
    print(json.dumps(report["summary"], indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
