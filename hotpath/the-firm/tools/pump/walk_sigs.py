#!/usr/bin/env python3
"""List finalized signatures that touch the 97e81 vault in (anchor, c829]. No RPC URLs."""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

VAULT_BASE = "b12785da6086afff2b88f4fadd2c0283675a673a4be2b3f19dd28062da808f51"
VAULT_QUOTE = "b28156655b46dee486cd59ea282ee22cdad4eb4bd29f517eb46bad92a4a419b0"
FROM_SLOT = 450882490
TO_SLOT = 450883470
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
        print("rpc_error", j["error"].get("message", "")[:200])
        return None
    return j.get("result")


def sigs_in_window(url: str, acct: str) -> list[dict]:
    out = []
    before = None
    pages = 0
    min_slot = None
    max_slot = None
    while pages < 200:
        opt = {"limit": 1000, "commitment": "finalized"}
        if before:
            opt["before"] = before
        batch = rpc(url, "getSignaturesForAddress", [acct, opt]) or []
        pages += 1
        if not batch:
            break
        for e in batch:
            slot = e.get("slot") or 0
            min_slot = slot if min_slot is None else min(min_slot, slot)
            max_slot = slot if max_slot is None else max(max_slot, slot)
        stop = False
        for e in batch:
            slot = e.get("slot") or 0
            if slot > TO_SLOT:
                continue
            if slot <= FROM_SLOT:
                stop = True
                break
            out.append(
                {
                    "slot": slot,
                    "err": e.get("err"),
                    "ok": e.get("err") is None,
                    "sig": e.get("signature"),
                }
            )
        before = batch[-1].get("signature")
        oldest = batch[-1].get("slot") or 0
        if stop or oldest <= FROM_SLOT:
            break
    print(f"  pages={pages} observed_slots=[{min_slot},{max_slot}] hit={len(out)}")
    return out


def b58decode(s: str) -> bytes:
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    pad = 0
    for c in s:
        if c == B58[0]:
            pad += 1
        else:
            break
    if n == 0:
        raw = b""
    else:
        raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return b"\x00" * pad + raw


def main() -> int:
    walk_path = Path(sys.argv[1] if len(sys.argv) > 1 else "run/ab1/walk97.jsonl")
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    seen = set()
    if walk_path.exists():
        for line in walk_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            h = json.loads(line).get("sig_hex") or ""
            if h:
                seen.add(b58encode(bytes.fromhex(h)))
    url = load_rpc()
    vb = b58encode(bytes.fromhex(VAULT_BASE))
    vq = b58encode(bytes.fromhex(VAULT_QUOTE))
    a = sigs_in_window(url, vb)
    b = sigs_in_window(url, vq)
    by_sig = {}
    for e in a + b:
        by_sig.setdefault(e["sig"], e)
    rows = sorted(by_sig.values(), key=lambda x: (x["slot"], x["sig"]))
    ok = [e for e in rows if e["ok"]]
    fail = [e for e in rows if not e["ok"]]
    missing = [e for e in ok if e["sig"] not in seen]
    print(f"chain_vault_sigs={len(rows)} ok={len(ok)} fail={len(fail)}")
    print(f"ok_not_in_walk={len(missing)} walk_hex_decoded={len(seen)}")
    if out_path is not None:
        with out_path.open("w", encoding="utf-8") as f:
            for e in missing:
                raw = b58decode(e["sig"])
                if len(raw) != 64:
                    print("bad_b58_len", len(raw), e["sig"][:16])
                    continue
                f.write(
                    json.dumps({"slot": e["slot"], "sig_hex": raw.hex(), "sig": e["sig"]})
                    + "\n"
                )
        print("wrote", out_path, "n", len(missing))
        return 0
    for e in ok:
        flag = "IN_WALK" if e["sig"] in seen else "MISSING_FROM_CAP_WALK"
        print(e["slot"], int(e["ok"]), flag, e["sig"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
