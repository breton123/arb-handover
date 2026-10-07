"""Resolve Helius RPC from env files. Never prints the key."""
from __future__ import annotations

import os
from pathlib import Path


def rpc_url() -> str:
    u = os.environ.get("HELIUS_RPC_URL") or os.environ.get("RPC_URL") or os.environ.get("HELIUS_RPC")
    if u:
        return u
    key = os.environ.get("HELIUS_API_KEY") or ""
    candidates = [
        Path.home() / ".arb-smoke.env",
        Path.home() / ".arb-state007.env",
        Path(r"c:\Users\louis\Desktop\TheMoneyMaker") / ".env",
        Path("/home/louis/TheMoneyMaker/.env"),
    ]
    for p in candidates:
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("HELIUS_RPC_URL=") or line.startswith("RPC_URL=") or line.startswith("HELIUS_RPC="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
            if line.startswith("HELIUS_API_KEY=") and not key:
                key = line.split("=", 1)[1].strip().strip('"').strip("'")
    if key:
        return "https://mainnet.helius-rpc.com/?api-key=" + key
    raise SystemExit("no HELIUS rpc in env")
