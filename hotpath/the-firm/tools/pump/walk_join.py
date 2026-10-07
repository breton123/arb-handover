#!/usr/bin/env python3
"""Join --walk-log with finalized getTransaction vault pre/post. Never prints RPC URLs."""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

POOL = "97e81bb1594ed2cb0757d9c8d65e9a01e6f2290af0b2e8ab3590bab163e5d735"
VAULT_BASE = "b12785da6086afff2b88f4fadd2c0283675a673a4be2b3f19dd28062da808f51"
VAULT_QUOTE = "b28156655b46dee486cd59ea282ee22cdad4eb4bd29f517eb46bad92a4a419b0"
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
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "anchor"))
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
        return None
    return j.get("result")


def keys_of(tx) -> list[str]:
    msg = (tx.get("transaction") or {}).get("message") or {}
    keys = []
    for k in msg.get("accountKeys") or []:
        keys.append(k.get("pubkey") if isinstance(k, dict) else str(k))
    la = (tx.get("meta") or {}).get("loadedAddresses") or {}
    if isinstance(la, dict):
        for k in la.get("writable") or []:
            keys.append(str(k))
        for k in la.get("readonly") or []:
            keys.append(str(k))
    return keys


def bal_for(entries, accs, want: str):
    for e in entries or []:
        idx = e.get("accountIndex")
        if idx is None or idx >= len(accs):
            continue
        if accs[idx] == want:
            amt = (e.get("uiTokenAmount") or {}).get("amount")
            return None if amt is None else int(amt)
    return None


def load_jsonl(path: Path) -> list[dict]:
    rows = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def main() -> int:
    walk_path = Path(sys.argv[1] if len(sys.argv) > 1 else "run/ab1/walk97.jsonl")
    cache_path = Path(sys.argv[2] if len(sys.argv) > 2 else "run/ab1/walk97_rpc.json")
    max_rpc = int(sys.argv[3]) if len(sys.argv) > 3 else 2500
    rows = load_jsonl(walk_path)
    vb = b58encode(bytes.fromhex(VAULT_BASE))
    vq = b58encode(bytes.fromhex(VAULT_QUOTE))
    pool = b58encode(bytes.fromhex(POOL))
    n_lut = sum(1 for r in rows if r.get("lut_miss"))
    n_touch = sum(
        1
        for r in rows
        if r.get("touch_pool") or r.get("touch_vault_base") or r.get("touch_vault_quote")
        or r.get("ours")
    )
    print(f"walk_rows={len(rows)} lut_miss={n_lut} resolved_touch={n_touch}")
    cache = {}
    if cache_path.exists():
        cache = json.loads(cache_path.read_text(encoding="utf-8"))
    url = load_rpc()
    fetched = 0
    for r in rows:
        sig_hex = r.get("sig_hex") or ""
        if not sig_hex:
            continue
        need = r.get("lut_miss") or r.get("touch_pool") or r.get("touch_vault_base")
        need = need or r.get("touch_vault_quote") or r.get("ours")
        if not need:
            continue
        if sig_hex in cache:
            continue
        if fetched >= max_rpc:
            print(f"rpc cap {max_rpc} reached")
            break
        sig = b58encode(bytes.fromhex(sig_hex))
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
        fetched += 1
        if res is None:
            cache[sig_hex] = {"missing": 1}
            continue
        meta = res.get("meta") or {}
        accs = keys_of(res)
        pre = meta.get("preTokenBalances") or []
        post = meta.get("postTokenBalances") or []
        cache[sig_hex] = {
            "slot": res.get("slot"),
            "err": meta.get("err"),
            "ok": meta.get("err") is None,
            "fee": meta.get("fee"),
            "has_pool": pool in accs,
            "has_vb": vb in accs,
            "has_vq": vq in accs,
            "vb_pre": bal_for(pre, accs, vb),
            "vb_post": bal_for(post, accs, vb),
            "vq_pre": bal_for(pre, accs, vq),
            "vq_post": bal_for(post, accs, vq),
        }
        if fetched % 50 == 0:
            cache_path.write_text(json.dumps(cache), encoding="utf-8")
            print(f"rpc {fetched} cached")
    cache_path.write_text(json.dumps(cache), encoding="utf-8")
    print(f"rpc_new={fetched} cache={len(cache)}")

    first_pre = None
    first_apply = None
    n_hidden_lut = 0
    n_chain_touch = 0
    print(
        "slot rc lut_miss ok chain_touch static_pool ours "
        "rb_pre chain_vb_pre vb_delta compact_delta"
    )
    for r in rows:
        sig = r.get("sig_hex") or ""
        info = cache.get(sig) or {}
        chain_touch = bool(info.get("has_pool") or info.get("has_vb") or info.get("has_vq"))
        if r.get("lut_miss") and chain_touch:
            n_hidden_lut += 1
        if chain_touch:
            n_chain_touch += 1
        ok = info.get("ok")
        vb_pre = info.get("vb_pre")
        vb_post = info.get("vb_post")
        compact_rb = r.get("rb_pre")
        chain_d = None
        if vb_pre is not None and vb_post is not None:
            chain_d = vb_post - vb_pre
        compact_d = None
        if r.get("rb_pre") is not None and r.get("rb_post") is not None:
            compact_d = r["rb_post"] - r["rb_pre"]
        if chain_touch and ok and vb_pre is not None and compact_rb is not None:
            if compact_rb != vb_pre and first_pre is None:
                first_pre = r, info
            if (
                compact_rb == vb_pre
                and compact_d is not None
                and chain_d is not None
                and compact_d != chain_d
                and first_apply is None
            ):
                first_apply = r, info
        if chain_touch or r.get("ours") or (r.get("lut_miss") and chain_touch):
            print(
                r.get("slot"),
                r.get("rc"),
                r.get("lut_miss"),
                ok,
                int(chain_touch),
                r.get("static_pool"),
                r.get("ours"),
                compact_rb,
                vb_pre,
                chain_d,
                compact_d,
                (sig or "")[:16],
            )

    print(f"chain_touch_txs={n_chain_touch} hidden_lut_touch={n_hidden_lut}")
    if first_pre:
        r, info = first_pre
        print("FIRST_PRESTATE_DIVERGE")
        print("  slot", r.get("slot"), "sig", r.get("sig_hex"))
        print("  rc", r.get("rc"), "lut_miss", r.get("lut_miss"), "ours", r.get("ours"))
        print("  compact_rb_pre", r.get("rb_pre"), "chain_vb_pre", info.get("vb_pre"))
        print("  compact_rq_pre", r.get("rq_pre"), "chain_vq_pre", info.get("vq_pre"))
        print("  static_pool", r.get("static_pool"), "touch_pool", r.get("touch_pool"))
    else:
        print("FIRST_PRESTATE_DIVERGE none_in_fetched_touching_ok")
    if first_apply:
        r, info = first_apply
        print("FIRST_APPLY_DIVERGE")
        print("  slot", r.get("slot"), "sig", r.get("sig_hex"), "rc", r.get("rc"))
        print("  compact_d_base", r.get("rb_post") - r.get("rb_pre"))
        print("  chain_d_base", (info.get("vb_post") or 0) - (info.get("vb_pre") or 0))
    else:
        print("FIRST_APPLY_DIVERGE none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
