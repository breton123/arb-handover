"""Load provider env. Never print tokens."""
from __future__ import annotations

import os
from pathlib import Path


def _load_file(path: Path) -> None:
    if not path.exists() or not path.is_file():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        if s.startswith("export "):
            s = s[7:].strip()
        k, v = s.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def load_env() -> None:
    here = Path(__file__).resolve()
    roots = [
        here.parents[2] / ".env",
        Path.home() / ".arb-state007.env",
        Path.home() / ".arb-smoke.env",
        Path(r"C:\Users\louis\Desktop\TheMoneyMaker") / ".env",
        Path.home() / "Desktop" / "TheMoneyMaker" / ".env",
    ]
    for p in roots:
        _load_file(p)
    if not os.environ.get("SHYFT_RPC") and os.environ.get("SHYFT_KEY"):
        os.environ["SHYFT_RPC"] = (
            "https://rpc.shyft.to?api_key=" + os.environ["SHYFT_KEY"]
        )


def rpc_url() -> str:
    return (
        os.environ.get("SHYFT_RPC")
        or os.environ.get("RPC_URL")
        or os.environ.get("HELIUS_RPC_URL")
        or ""
    ).strip()


def grpc_host() -> str:
    raw = (
        os.environ.get("SHYFT_GRPC_URL")
        or os.environ.get("GRPC_URL")
        or "https://grpc.fra.shyft.to"
    ).strip()
    raw = raw.replace("https://", "").replace("http://", "")
    if raw and ":" not in raw:
        raw = raw + ":443"
    return raw


def grpc_token() -> str:
    return (
        os.environ.get("SHYFT_X_TOKEN")
        or os.environ.get("SHYFT_TOKEN")
        or os.environ.get("X_TOKEN")
        or ""
    ).strip()
