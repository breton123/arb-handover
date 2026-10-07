#!/usr/bin/env python3
"""One-shot: finalized getTransaction for c829. Never prints RPC URLs or keys."""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

HEX = (
    "c829b8b5704a003eeb68d43c1c7e3e5b502211c7adf8e06e1acc3906bf6810f5"
    "5a08883cb0bd9ba55d7790957eddbb953abffd20d2d4571f276cfbb690c44309"
)
VAULT_BASE = "b12785da6086afff2b88f4fadd2c0283675a673a4be2b3f19dd28062da808f51"
VAULT_QUOTE = "b28156655b46dee486cd59ea282ee22cdad4eb4bd29f517eb46bad92a4a419b0"
POOL = "97e81bb1594ed2cb0757d9c8d65e9a01e6f2290af0b2e8ab3590bab163e5d735"

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58encode(data: bytes) -> str:
    n = int.from_bytes(data, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    pad = 0
    for b in data:
        if b == 0:
            pad += 1
        else:
            break
    return B58[0] * pad + (out or "")


def load_rpc() -> str:
    here = Path(__file__).resolve()
    sys.path.insert(0, str(here.parents[1] / "anchor"))
    from envload import load_env, rpc_url  # noqa: E402

    load_env()
    u = rpc_url().strip()
    if not u:
        sys.exit("no rpc")
    return u


def rpc(url: str, method: str, params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
    req = urllib.request.Request(
        url, data=body.encode(), headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=45) as resp:
        j = json.load(resp)
    if j.get("error"):
        print("rpc_error", j["error"].get("code"), j["error"].get("message", "")[:200])
        return None
    return j.get("result")


def keys_of(tx) -> list[str]:
    msg = (tx.get("transaction") or {}).get("message") or {}
    keys = list(msg.get("accountKeys") or [])
    loaded = []
    meta = tx.get("meta") or {}
    for group in meta.get("loadedAddresses") or {}:
        if isinstance(group, list):
            loaded.extend(group)
    la = meta.get("loadedAddresses") or {}
    if isinstance(la, dict):
        loaded.extend(la.get("writable") or [])
        loaded.extend(la.get("readonly") or [])
    out = []
    for k in keys + loaded:
        if isinstance(k, dict):
            out.append(k.get("pubkey") or "")
        else:
            out.append(str(k))
    return out


def main() -> int:
    url = load_rpc()
    sig = b58encode(bytes.fromhex(HEX))
    print("sig_b58", sig)
    res = rpc(
        url,
        "getTransaction",
        [
            sig,
            {
                "encoding": "json",
                "maxSupportedTransactionVersion": 0,
                "commitment": "finalized",
            },
        ],
    )
    if res is None:
        print("result=null")
        return 1
    meta = res.get("meta") or {}
    err = meta.get("err")
    print("slot", res.get("slot"))
    print("blockTime", res.get("blockTime"))
    print("err", json.dumps(err, separators=(",", ":")))
    print("ok", err is None)
    print("fee", meta.get("fee"))
    print("cu", meta.get("computeUnitsConsumed"))
    print("inner_groups", len(meta.get("innerInstructions") or []))
    logs = meta.get("logMessages") or []
    print("nlogs", len(logs))
    want = ("slip", "exceed", "error", "failed", "custom", "600")
    for line in logs:
        low = line.lower()
        if any(k in low for k in want) or "Instruction:" in line:
            print("log", line)
    vb = b58encode(bytes.fromhex(VAULT_BASE))
    vq = b58encode(bytes.fromhex(VAULT_QUOTE))
    pool = b58encode(bytes.fromhex(POOL))
    accs = keys_of(res)
    print("has_pool", pool in accs)
    print("has_vault_base", vb in accs)
    print("has_vault_quote", vq in accs)
    pre = meta.get("preTokenBalances") or []
    post = meta.get("postTokenBalances") or []

    def bal_for(entries, want_b58):
        for e in entries or []:
            idx = e.get("accountIndex")
            if idx is None or idx >= len(accs):
                continue
            if accs[idx] == want_b58:
                return (e.get("uiTokenAmount") or {}).get("amount")
        return None

    print("vault_base_pre", bal_for(pre, vb), "post", bal_for(post, vb))
    print("vault_quote_pre", bal_for(pre, vq), "post", bal_for(post, vq))
    print("token_deltas")
    pre_map = {}
    post_map = {}
    for e in pre:
        idx = e.get("accountIndex")
        if idx is not None:
            pre_map[idx] = (e.get("uiTokenAmount") or {}).get("amount")
    for e in post:
        idx = e.get("accountIndex")
        if idx is not None:
            post_map[idx] = (e.get("uiTokenAmount") or {}).get("amount")
    nacc = len(accs)
    for idx in sorted(set(pre_map) | set(post_map)):
        a = pre_map.get(idx)
        b = post_map.get(idx)
        if a == b:
            continue
        pk = accs[idx] if idx < nacc else "?"
        tag = ""
        if idx < nacc:
            if accs[idx] == vb:
                tag = " VAULT_BASE"
            elif accs[idx] == vq:
                tag = " VAULT_QUOTE"
            elif accs[idx] == pool:
                tag = " POOL"
        print("  idx", idx, "pre", a, "post", b, pk, tag)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
