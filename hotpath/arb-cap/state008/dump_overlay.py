#!/usr/bin/env python3
"""Dump the full instruction tree for a STATE-008 shadow mismatch. Read-only RPC."""
from __future__ import annotations

import json
import os
import sys
import urllib.request
from pathlib import Path

DLMM = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
SWAP2 = bytes.fromhex("414b3f4ceb5b5b88")
SWAP1 = bytes.fromhex("f8c69e91e17587c8")
SELL = bytes.fromhex("33e685a4017f83ad")
BUYEQ = bytes.fromhex("c62e1552b4d9e870")
BUY = bytes.fromhex("66063d1201daebea")

EXACT = {
    SWAP2: "dlmm_swap2",
    SWAP1: "dlmm_swap1",
    SELL: "pump_sell",
    BUYEQ: "pump_buy_exact_quote",
}


def rpc_url() -> str:
    homes = [
        Path.home() / ".arb-state007.env",
        Path.home() / ".env",
        Path("/home/louis/TheMoneyMaker/.env"),
        Path(r"c:\Users\louis\Desktop\TheMoneyMaker\.env"),
    ]
    for p in homes:
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            if line.startswith("SHYFT_KEY="):
                return "https://rpc.shyft.to?api_key=" + line.split("=", 1)[1].strip()
            if line.startswith("HELIUS_RPC_URL="):
                return line.split("=", 1)[1].strip()
            if line.startswith("RPC_URL="):
                return line.split("=", 1)[1].strip()
    key = os.environ.get("SHYFT_KEY") or os.environ.get("HELIUS_API_KEY")
    if os.environ.get("SHYFT_KEY"):
        return "https://rpc.shyft.to?api_key=" + os.environ["SHYFT_KEY"]
    if os.environ.get("HELIUS_RPC_URL"):
        return os.environ["HELIUS_RPC_URL"]
    raise SystemExit("no rpc")


def rpc(url: str, method: str, params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=45) as r:
        data = json.loads(r.read().decode())
    if data.get("error"):
        raise SystemExit(str(data["error"]))
    return data.get("result")


def b58_keys(msg: dict) -> list[str]:
    keys = list(msg.get("accountKeys") or [])
    out = []
    for k in keys:
        if isinstance(k, dict):
            out.append(k.get("pubkey") or "")
        else:
            out.append(str(k))
    loaded = (msg.get("loadedAddresses") or {})
    out.extend(loaded.get("writable") or [])
    out.extend(loaded.get("readonly") or [])
    return out


def ix_row(ix: dict, keys: list[str], inner: bool) -> dict:
    prog = ix.get("programId") or ""
    if not prog and "programIdIndex" in ix:
        idx = int(ix["programIdIndex"])
        prog = keys[idx] if idx < len(keys) else ""
    parsed = ix.get("parsed") if isinstance(ix.get("parsed"), dict) else None
    data = ix.get("data") or ""
    raw = b""
    if isinstance(data, str) and data:
        alph = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
        n = 0
        ok = True
        for ch in data:
            i = alph.find(ch)
            if i < 0:
                ok = False
                break
            n = n * 58 + i
        if ok and n:
            raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    disc = raw[:8].hex() if len(raw) >= 8 else ""
    name = EXACT.get(raw[:8], None) if len(raw) >= 8 else None
    accs = []
    for a in ix.get("accounts") or []:
        if isinstance(a, dict):
            accs.append(a.get("pubkey") or "")
        elif isinstance(a, int) and a < len(keys):
            accs.append(keys[a])
        else:
            accs.append(str(a))
    kind = "other"
    if prog == DLMM:
        kind = name or ("dlmm_unknown_disc" if disc else "dlmm")
    elif prog == PUMP:
        kind = name or ("pump_unknown_disc" if disc else "pump")
    elif prog.endswith("11111111111111111111111111111111") or "ComputeBudget" in str(ix.get("program")):
        kind = "sys"
    return {
        "inner": inner,
        "program": prog,
        "kind": kind,
        "disc": disc,
        "dlen": len(raw),
        "accounts": accs[:16],
        "exact": name is not None,
    }


def main() -> int:
    def b58enc(raw: bytes) -> str:
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
        return ("1" * pad) + (out or "1")

    raw_sig = sys.argv[1] if len(sys.argv) > 1 else (
        "5dc37b6cdb71039203995e3ccb2c3fadf4e3cff50e51af98cf61f0c3075df09f"
        "798f04826fd1eba078527830ad361e5c4cde5319ec60a7881d9fdc20fe56bf0f"
    )
    sig = raw_sig
    if len(raw_sig) == 128 and all(c in "0123456789abcdefABCDEF" for c in raw_sig):
        sig = b58enc(bytes.fromhex(raw_sig))
    pool = sys.argv[2] if len(sys.argv) > 2 else "GYqytuXSiX3GaPuCkLMY4jeRR3mAtyAuQqhD32d45g5y"
    url = rpc_url()
    tx = rpc(url, "getTransaction", [sig, {
        "encoding": "json",
        "maxSupportedTransactionVersion": 1,
        "commitment": "confirmed",
    }])
    if not isinstance(tx, dict):
        raise SystemExit("empty tx")
    tr = tx.get("transaction") or {}
    msg = tr.get("message") or {}
    meta = tx.get("meta") or {}
    keys = b58_keys(msg)
    rows = []
    for ix in msg.get("instructions") or []:
        rows.append(ix_row(ix, keys, False))
    for group in meta.get("innerInstructions") or []:
        for ix in group.get("instructions") or []:
            rows.append(ix_row(ix, keys, True))
    logs = []
    for line in meta.get("logMessages") or []:
        if "Instruction: " in line:
            logs.append(line.split("Instruction: ", 1)[1].strip())
    pricing = [r for r in rows if r["kind"].startswith("dlmm") or r["kind"].startswith("pump")]
    on_pool = [r for r in pricing if pool in r["accounts"]]
    unknown = [r for r in on_pool if not r["exact"]]
    out = {
        "sig": sig,
        "slot": tx.get("slot"),
        "err": meta.get("err"),
        "n_outer": len(msg.get("instructions") or []),
        "n_inner": sum(len(g.get("instructions") or []) for g in (meta.get("innerInstructions") or [])),
        "log_ixs": logs,
        "outer_ixs": [r for r in rows if not r["inner"]],
        "pricing_ixs": pricing,
        "on_pool": on_pool,
        "unknown_on_pool": unknown,
        "account_writes": [k for i, k in enumerate(keys) if i < 64],
    }
    dest = Path("/home/louis/captures/state008/TX_OVERLAY_001.json")
    try:
        dest.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    except Exception:
        dest = Path("TX_OVERLAY_001.json")
        dest.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "slot": out["slot"], "err": out["err"],
        "n_outer": out["n_outer"], "n_inner": out["n_inner"],
        "log_ixs": logs, "on_pool": [r["kind"] for r in on_pool],
        "unknown_on_pool": [r["kind"] for r in unknown],
        "wrote": str(dest),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
