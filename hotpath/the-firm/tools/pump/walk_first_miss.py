#!/usr/bin/env python3
"""Finalized vault pre of the first missing post-anchor vault tx. No RPC URLs."""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

SIG = "17rz7FhLbDRUoczfr4SJYgv76y39nXSj2NdfbSGvsevZFwEEZ5ju9gTFZ1CK3KKyBWRiwH3ZftLsz9AcFMG1XVE"
VAULT_BASE = "b12785da6086afff2b88f4fadd2c0283675a673a4be2b3f19dd28062da808f51"
VAULT_QUOTE = "b28156655b46dee486cd59ea282ee22cdad4eb4bd29f517eb46bad92a4a419b0"
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
SNAP_RB = 68032410436495
SNAP_RQ = 241232166150


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
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "anchor"))
    from envload import load_env, rpc_url  # noqa: E402

    load_env()
    u = rpc_url().strip()
    if not u:
        sys.exit("no rpc")
    return u


def rpc(url, method, params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
    req = urllib.request.Request(
        url, data=body.encode(), headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=45) as resp:
        j = json.load(resp)
    return j.get("result")


def keys_of(tx):
    msg = (tx.get("transaction") or {}).get("message") or {}
    keys = []
    for k in msg.get("accountKeys") or []:
        keys.append(k.get("pubkey") if isinstance(k, dict) else str(k))
    la = (tx.get("meta") or {}).get("loadedAddresses") or {}
    if isinstance(la, dict):
        keys.extend(str(x) for x in (la.get("writable") or []))
        keys.extend(str(x) for x in (la.get("readonly") or []))
    return keys


def bal(entries, accs, want):
    for e in entries or []:
        idx = e.get("accountIndex")
        if idx is not None and idx < len(accs) and accs[idx] == want:
            amt = (e.get("uiTokenAmount") or {}).get("amount")
            return None if amt is None else int(amt)
    return None


def main() -> int:
    url = load_rpc()
    res = rpc(
        url,
        "getTransaction",
        [SIG, {"encoding": "json", "maxSupportedTransactionVersion": 0, "commitment": "finalized"}],
    )
    if not res:
        print("missing")
        return 1
    meta = res.get("meta") or {}
    accs = keys_of(res)
    vb = b58encode(bytes.fromhex(VAULT_BASE))
    vq = b58encode(bytes.fromhex(VAULT_QUOTE))
    pre = meta.get("preTokenBalances") or []
    vb_pre = bal(pre, accs, vb)
    vq_pre = bal(pre, accs, vq)
    print("slot", res.get("slot"), "ok", meta.get("err") is None)
    print("vb_pre", vb_pre, "snap_rb", SNAP_RB, "match", vb_pre == SNAP_RB)
    print("vq_pre", vq_pre, "snap_rq", SNAP_RQ, "match", vq_pre == SNAP_RQ)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
