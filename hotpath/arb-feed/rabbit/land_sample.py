#!/usr/bin/env python3
"""Off-path eventual-commit sample. Never run on the Rabbit RX thread."""
from __future__ import annotations

import json
import os
import time
import urllib.request
from pathlib import Path

RACE = Path("/home/louis/captures/rabbit/race.jsonl")
OUT = Path("/home/louis/captures/rabbit/OUTCOME.json")
ENV = Path.home() / ".arb-smoke.env"


def _rpc() -> str:
    env = {}
    if ENV.exists():
        for line in ENV.read_text(encoding="utf-8").splitlines():
            line = line.strip().strip("\r")
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip().strip("\r")
    url = (
        os.environ.get("HELIUS_RPC")
        or os.environ.get("RPC_URL")
        or env.get("HELIUS_RPC")
        or env.get("RPC_URL")
        or env.get("HELIUS_URL")
        or ""
    )
    if not url:
        key = os.environ.get("HELIUS_API_KEY") or env.get("HELIUS_API_KEY") or ""
        if key:
            url = f"https://beta.helius-rpc.com/?api-key={key}"
    if not url:
        raise SystemExit("no RPC url in env")
    return url


def _b58(sig_hex: str) -> str:
    import sys
    sys.path.insert(0, "/home/louis/arb-cap")
    import record_dlmm as d
    return d._pk(bytes.fromhex(sig_hex))


def _post(url: str, method: str, params) -> dict:
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode())


def classify_status(st) -> str:
    if not st:
        return "NOT_SEEN"
    err = st.get("err")
    if err is None:
        return "LANDED_SUCCESS"
    return "LANDED_FAILED"


def main() -> int:
    if not RACE.exists():
        print("no race.jsonl")
        return 1
    rows = [json.loads(l) for l in RACE.read_text(encoding="utf-8").splitlines() if l.strip()]
    matched = [r for r in rows if r.get("lead_ns") is not None]
    # Prefer economically relevant, then fill.
    prefer = [r for r in matched if r.get("searchable") or r.get("known") or r.get("proto") in ("dlmm", "pump")]
    rest = [r for r in matched if r not in prefer]
    sample = (prefer + rest)[:80]
    url = _rpc()
    buckets = {
        "LANDED_SUCCESS": 0,
        "LANDED_FAILED": 0,
        "NOT_SEEN": 0,
        "n": 0,
        "by": {
            "both": {"LANDED_SUCCESS": 0, "LANDED_FAILED": 0, "NOT_SEEN": 0, "n": 0},
            "dlmm": {"LANDED_SUCCESS": 0, "LANDED_FAILED": 0, "NOT_SEEN": 0, "n": 0},
            "pump": {"LANDED_SUCCESS": 0, "LANDED_FAILED": 0, "NOT_SEEN": 0, "n": 0},
            "known": {"LANDED_SUCCESS": 0, "LANDED_FAILED": 0, "NOT_SEEN": 0, "n": 0},
        },
    }
    for r in sample:
        try:
            sig = _b58(r["sig"])
        except Exception:
            buckets["NOT_SEEN"] += 1
            continue
        try:
            js = _post(url, "getSignatureStatuses", [[sig], {"searchTransactionHistory": True}])
            st = ((js.get("result") or {}).get("value") or [None])[0]
            cls = classify_status(st)
        except Exception:
            cls = "NOT_SEEN"
        buckets[cls] += 1
        buckets["n"] += 1
        for tag, pred in (
            ("both", True),
            ("dlmm", r.get("proto") == "dlmm"),
            ("pump", r.get("proto") == "pump"),
            ("known", bool(r.get("known"))),
        ):
            if pred:
                buckets["by"][tag][cls] += 1
                buckets["by"][tag]["n"] += 1
        time.sleep(0.05)
    OUT.write_text(json.dumps(buckets, indent=2) + "\n", encoding="utf-8")
    print(
        f"OUTCOME n={buckets['n']} ok={buckets['LANDED_SUCCESS']} "
        f"fail={buckets['LANDED_FAILED']} unseen={buckets['NOT_SEEN']}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
