#!/usr/bin/env python3
"""PAPER-004 — validate the 34 contemporaneous decisions + classify missing-state.

RPC is observation only. No send. Never prints secrets.
"""
from __future__ import annotations

import json
import os
import statistics
import sys
import time
import urllib.error
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

SOL = "So11111111111111111111111111111111111111112"
DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
EST_FEE_LAMPORTS = 105_000  # 5k base + 100k priority band
LAMPORTS = 1_000_000_000


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
    return "1" * pad + (out or "1")


def b58decode(s: str) -> bytes:
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


def hex_pk(h: str) -> str:
    if not h:
        return ""
    return b58encode(bytes.fromhex(h))


def rpc_url() -> str:
    u = (os.environ.get("HELIUS_RPC_URL") or os.environ.get("RPC_URL") or "").strip()
    if u:
        return u
    k = (os.environ.get("HELIUS_API_KEY") or "").strip()
    if not k:
        raise SystemExit("set HELIUS_API_KEY or RPC_URL")
    return f"https://mainnet.helius-rpc.com/?api-key={k}"


def rpc(method: str, params, pause=0.08):
    url = rpc_url()
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    last = None
    for _ in range(8):
        time.sleep(pause)
        req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
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
    raise RuntimeError(last or "rpc fail")


def sol_px() -> float:
    try:
        req = urllib.request.Request(
            "https://api.binance.com/api/v3/ticker/price?symbol=SOLUSDT",
            headers={"User-Agent": "paper004"},
        )
        with urllib.request.urlopen(req, timeout=10) as r:
            return float(json.loads(r.read().decode())["price"])
    except Exception:
        return 160.0


def cycle_ain_is_sol(row: dict) -> bool:
    if row.get("opp_dir") == 1:
        return bool(row.get("sol_x") or row.get("sol_y"))
    return bool(row.get("sol_x"))


def gp_as_sol(row: dict) -> float | None:
    gp = int(row.get("opp_gp") or 0)
    ain = int(row.get("opp_ain") or 0)
    if gp <= 0:
        return None
    if cycle_ain_is_sol(row) or (10**8 <= ain <= 100 * LAMPORTS):
        return gp / LAMPORTS
    return None


def token_amt(data: bytes) -> int | None:
    if data is None or len(data) < 72:
        return None
    return int.from_bytes(data[64:72], "little")


def tx_keys(tx: dict) -> list[str]:
    msg = ((tx.get("transaction") or {}).get("message") or {})
    keys = list(msg.get("accountKeys") or [])
    out = []
    for k in keys:
        out.append(k.get("pubkey") if isinstance(k, dict) else str(k))
    return [x for x in out if x]


def ix_pids(tx: dict) -> set[str]:
    keys = tx_keys(tx)
    pids: set[str] = set()
    msg = ((tx.get("transaction") or {}).get("message") or {})
    for ix in msg.get("instructions") or []:
        pid = ix.get("programId")
        if pid is None:
            i = ix.get("programIdIndex")
            if i is not None and i < len(keys):
                pid = keys[i]
        if pid:
            pids.add(pid)
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        for ix in g.get("instructions") or []:
            pid = ix.get("programId")
            if pid is None:
                i = ix.get("programIdIndex")
                if i is not None and i < len(keys):
                    pid = keys[i]
            if pid:
                pids.add(pid)
    return pids


def load_dec(path: Path) -> list[dict]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def vault_amount(pk_b58: str) -> tuple[int | None, int | None]:
    accs = d.get_multiple([pk_b58])
    acc = accs[0] if accs else None
    if not acc:
        return None, None
    data = acc["data"]
    slot = acc.get("slot")
    return token_amt(data) if isinstance(data, (bytes, bytearray)) else None, slot


def fetch_tx(sig: str):
    return rpc(
        "getTransaction",
        [sig, {"encoding": "jsonParsed", "maxSupportedTransactionVersion": 1, "commitment": "confirmed"}],
    )


def pre_post_vault(tx: dict, vault: str) -> tuple[int | None, int | None]:
    meta = tx.get("meta") or {}
    pre = meta.get("preTokenBalances") or []
    post = meta.get("postTokenBalances") or []
    keys = tx_keys(tx)

    def find(bals):
        for b in bals:
            acct = b.get("owner")
            idx = b.get("accountIndex")
            pk = keys[idx] if idx is not None and idx < len(keys) else ""
            if pk == vault or acct == vault:
                ui = ((b.get("uiTokenAmount") or {}).get("amount"))
                if ui is not None:
                    return int(ui)
        return None

    return find(pre), find(post)


def later_arbs(pool: str, n_sig: str, n_slot: int) -> dict:
    try:
        sigs = rpc(
            "getSignaturesForAddress",
            [pool, {"limit": 40}],
            pause=0.15,
        ) or []
    except Exception as e:
        return {"error": str(e)}
    after = []
    seen_n = False
    for ent in sigs:
        sig = ent.get("signature")
        if sig == n_sig:
            seen_n = True
            continue
        if not seen_n:
            sl = int(ent.get("slot") or 0)
            if sl >= n_slot:
                after.append(ent)
        if len(after) >= 8:
            break
    hit = None
    for ent in reversed(after):
        try:
            tx = fetch_tx(ent["signature"])
        except Exception:
            continue
        if not tx or (tx.get("meta") or {}).get("err"):
            continue
        pids = ix_pids(tx)
        if DLMM in pids and PUMP in pids:
            keys = tx_keys(tx)
            hit = {
                "sig": ent["signature"],
                "slot": ent.get("slot"),
                "searcher": keys[0] if keys else "",
                "tx_index": (tx.get("transaction") or {}).get("index"),
                "same_slot": int(ent.get("slot") or 0) == n_slot,
            }
            break
    return {"later_scanned": len(after), "arb": hit}


def main() -> int:
    live.load_dotenv()
    dec_path = Path(sys.argv[1] if len(sys.argv) > 1 else "paper004_dec.jsonl")
    out_path = Path(sys.argv[2] if len(sys.argv) > 2 else dec_path.parent / "PAPER004.json")
    rows = load_dec(dec_path)
    px = sol_px()
    why = Counter(r.get("why") for r in rows)
    opps = [r for r in rows if r.get("opp_valid")]
    print(f"PAPER-004  dec={len(rows)} opp={len(opps)} sol_px={px:.2f}", flush=True)
    print("  why", dict(why), flush=True)

    sols = []
    usd = []
    for r in opps:
        g = gp_as_sol(r)
        r["predicted_gross_sol"] = g
        r["predicted_gross_usd"] = (g * px) if g is not None else None
        r["est_fee_sol"] = EST_FEE_LAMPORTS / LAMPORTS
        r["predicted_net_sol"] = (g - EST_FEE_LAMPORTS / LAMPORTS) if g is not None else None
        r["predicted_net_usd"] = (
            (g - EST_FEE_LAMPORTS / LAMPORTS) * px if g is not None else None
        )
        r["sig_b58"] = b58encode(bytes.fromhex(r["sig"])) if r.get("sig") else ""
        r["pool_b58"] = hex_pk(r.get("pool") or "")
        if g is not None:
            sols.append(g)
            usd.append(g * px)

    def pct(xs, p):
        if not xs:
            return None
        s = sorted(xs)
        i = int(p * (len(s) - 1))
        return s[i]

    dist = {
        "n": len(opps),
        "sol_px": px,
        "gross_sol_p50": pct(sols, 0.50),
        "gross_sol_p90": pct(sols, 0.90),
        "gross_sol_max": max(sols) if sols else None,
        "gross_usd_p50": pct(usd, 0.50),
        "gross_usd_p90": pct(usd, 0.90),
        "gross_usd_max": max(usd) if usd else None,
        "net_usd_p50": pct([(g - EST_FEE_LAMPORTS / LAMPORTS) * px for g in sols], 0.50)
        if sols
        else None,
        "tiny_lt_1c": sum(1 for x in usd if x < 0.01),
        "mid_1c_1d": sum(1 for x in usd if 0.01 <= x < 1),
        "big_ge_1d": sum(1 for x in usd if x >= 1),
    }
    print("  dist", json.dumps(dist), flush=True)

    chain = []
    s_match = {"checked": 0, "eq": 0, "close": 0, "mismatch": 0, "n_unseen": 0, "n_fail": 0}
    for i, r in enumerate(opps):
        sig = r["sig_b58"]
        rec = {
            "i": i,
            "sig": sig,
            "pool": r["pool_b58"],
            "pool_idx": r.get("pool_idx"),
            "pool_kind": r.get("pool_kind"),
            "state_version": r.get("state_version"),
            "n_dir": r.get("n_dir"),
            "n_ain": r.get("n_ain"),
            "our_dir": r.get("opp_dir"),
            "our_ain": r.get("opp_ain"),
            "predicted_gross_sol": r.get("predicted_gross_sol"),
            "predicted_gross_usd": r.get("predicted_gross_usd"),
            "predicted_net_usd": r.get("predicted_net_usd"),
            "ts_ns": r.get("ts_ns"),
            "why": r.get("why"),
        }
        try:
            st = rpc("getSignatureStatuses", [[sig], {"searchTransactionHistory": True}])
            ent = ((st or {}).get("value") or [None])[0]
        except Exception as e:
            rec["rpc_err"] = str(e)
            chain.append(rec)
            continue
        if not ent:
            rec["n_landed"] = False
            s_match["n_unseen"] += 1
            chain.append(rec)
            print(f"  {i+1}/34 unseen {sig[:8]}", flush=True)
            continue
        rec["n_landed"] = ent.get("err") is None
        rec["slot"] = ent.get("slot")
        rec["confirm"] = ent.get("confirmationStatus")
        if ent.get("err") is not None:
            rec["n_err"] = ent.get("err")
            s_match["n_fail"] += 1
            chain.append(rec)
            continue
        try:
            tx = fetch_tx(sig)
        except Exception as e:
            rec["tx_err"] = str(e)
            chain.append(rec)
            continue
        if not tx:
            rec["n_landed"] = False
            s_match["n_unseen"] += 1
            chain.append(rec)
            continue
        rec["slot"] = tx.get("slot")
        rec["block_time"] = tx.get("blockTime")
        rec["tx_index"] = tx.get("index")
        rec["first_shred_ns"] = r.get("ts_ns")
        vx = hex_pk(r.get("vault_x") or "")
        vy = hex_pk(r.get("vault_y") or "")
        sprime = r.get("sprime") or {}
        actual = {}
        eq = None
        if r.get("pool_kind") == "dlmm" and vx and vy:
            px0, px1 = pre_post_vault(tx, vx)
            py0, py1 = pre_post_vault(tx, vy)
            actual = {
                "vault_x_pre": px0,
                "vault_x_post": px1,
                "vault_y_pre": py0,
                "vault_y_post": py1,
            }
            try:
                accs = d.get_multiple([r["pool_b58"]])
                lb = d.parse_lbpair(accs[0]["data"]) if accs and accs[0] else None
                if lb:
                    actual["chain_active_id"] = lb["active_id"]
            except Exception:
                pass
            if sprime and px1 is not None and py1 is not None:
                s_match["checked"] += 1
                rx, ry = int(sprime.get("reserve_x") or 0), int(sprime.get("reserve_y") or 0)
                dx, dy = abs(rx - px1), abs(ry - py1)
                rec["s_prime_vs_post"] = {
                    "pred_rx": rx,
                    "pred_ry": ry,
                    "post_rx": px1,
                    "post_ry": py1,
                    "d_rx": dx,
                    "d_ry": dy,
                    "pred_active": sprime.get("active_id"),
                    "chain_active": actual.get("chain_active_id"),
                }
                if dx == 0 and dy == 0:
                    eq = True
                    s_match["eq"] += 1
                elif dx * 10000 < max(px1, 1) and dy * 10000 < max(py1, 1):
                    eq = "close"
                    s_match["close"] += 1
                else:
                    eq = False
                    s_match["mismatch"] += 1
        elif r.get("pool_kind") == "pump" and vx and vy:
            pb0, pb1 = pre_post_vault(tx, vx)
            pq0, pq1 = pre_post_vault(tx, vy)
            actual = {"base_pre": pb0, "base_post": pb1, "quote_pre": pq0, "quote_post": pq1}
            if sprime and pb1 is not None and pq1 is not None:
                s_match["checked"] += 1
                rb, rq = int(sprime.get("reserve_base") or 0), int(sprime.get("reserve_quote") or 0)
                rec["s_prime_vs_post"] = {
                    "pred_base": rb,
                    "pred_quote": rq,
                    "post_base": pb1,
                    "post_quote": pq1,
                    "d_base": abs(rb - pb1),
                    "d_quote": abs(rq - pq1),
                }
                if rb == pb1 and rq == pq1:
                    eq = True
                    s_match["eq"] += 1
                else:
                    eq = False
                    s_match["mismatch"] += 1
        rec["s_prime_eq"] = eq
        rec["actual_post"] = actual
        rec["subsequent"] = later_arbs(r["pool_b58"], sig, int(rec.get("slot") or 0))
        chain.append(rec)
        print(
            f"  {i+1}/34 land={rec.get('n_landed')} s'={eq} "
            f"gp_usd={r.get('predicted_gross_usd')} {sig[:8]}",
            flush=True,
        )

    report = {
        "decisions": len(rows),
        "opp": len(opps),
        "why": dict(why),
        "state_sufficient_replay": why.get("OK", 0) + why.get("NO_OPP", 0),
        "missing_state_replay": len(rows) - why.get("OK", 0) - why.get("NO_OPP", 0),
        "distribution": dist,
        "s_prime": s_match,
        "chain": chain,
        "send": False,
        "note": (
            "predicted S' from replaying N on journaled S(version). "
            "actual post-N from landed tx vault pre/post. "
            "gross is cycle_size output; treated as SOL when ain is on the SOL ladder "
            f"or sol-side mint. est fee {EST_FEE_LAMPORTS} lamports. No Flowra."
        ),
    }
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("why", "distribution", "s_prime")}, indent=2))
    print(f"wrote {out_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
