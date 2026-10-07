"""Shared Shyft JSON-RPC scheduler. Cold-start / repair only. Never prints keys."""
from __future__ import annotations

import json
import os
import random
import time
import urllib.error
import urllib.request
from pathlib import Path

class RateLimit(Exception):
    pass


CHANNELS = ("RPC_BOOTSTRAP", "RPC_REPAIR", "RPC_ALT_WARM", "RPC_MISC")
MAX_KEYS = 100
TARGET_RPS = 75.0
STATE = Path(os.environ.get("RPC_SCHED_STATE") or (Path.home() / "captures" / "rpc_sched.state"))
MET = Path(os.environ.get("RPC_SCHED_MET") or (Path.home() / "captures" / "rpc_sched.json"))

try:
    import fcntl
except ImportError:
    fcntl = None


def _env_files() -> list[Path]:
    return [
        Path.home() / ".arb-smoke.env",
        Path.home() / ".arb-state007.env",
        Path("/home/louis/TheMoneyMaker/.env"),
        Path(r"c:\Users\louis\Desktop\TheMoneyMaker") / ".env",
    ]


def _read_env(name: str) -> str:
    v = os.environ.get(name) or ""
    if v:
        return v.strip().strip('"').strip("'")
    for p in _env_files():
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    return ""


def rpc_url() -> str:
    url = _read_env("SHYFT_RPC_URL")
    if url:
        return url
    key = _read_env("SHYFT_KEY") or _read_env("SHYFT_API_KEY")
    if key:
        return "https://rpc.shyft.to?api_key=" + key
    raise RuntimeError("SHYFT_RPC_URL or SHYFT_KEY required for AUTH/ALT RPC")


def rpc_host() -> str:
    try:
        from urllib.parse import urlparse
        return urlparse(rpc_url()).netloc or "rpc.shyft.to"
    except Exception:
        return "rpc.shyft.to"


def _lock_file(fh):
    if fcntl is not None:
        fcntl.flock(fh.fileno(), fcntl.LOCK_EX)


def _unlock_file(fh):
    if fcntl is not None:
        fcntl.flock(fh.fileno(), fcntl.LOCK_UN)


def acquire(rps: float = TARGET_RPS) -> None:
    """Cross-process leaky interval. Headroom under a 100 rps plan."""
    interval = 1.0 / max(1.0, rps)
    STATE.parent.mkdir(parents=True, exist_ok=True)
    with STATE.open("a+", encoding="utf-8") as fh:
        _lock_file(fh)
        try:
            fh.seek(0)
            raw = fh.read().strip()
            last = float(raw) if raw else 0.0
            now = time.time()
            wait = last + interval - now
            if wait > 0:
                time.sleep(wait)
                now = time.time()
            fh.seek(0)
            fh.truncate()
            fh.write(f"{now:.6f}\n")
            fh.flush()
        finally:
            _unlock_file(fh)


def _note(channel: str, method: str, ok: bool, nkeys: int = 0) -> None:
    if channel not in CHANNELS:
        channel = "RPC_MISC"
    MET.parent.mkdir(parents=True, exist_ok=True)
    doc = {"host": rpc_host(), "rps_target": TARGET_RPS, "by": {}}
    try:
        if MET.exists():
            doc.update(json.loads(MET.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    by = doc.setdefault("by", {})
    row = by.setdefault(channel, {"ok": 0, "fail": 0, "keys": 0, "methods": {}})
    row["ok" if ok else "fail"] += 1
    row["keys"] += nkeys
    row["methods"][method] = row["methods"].get(method, 0) + 1
    doc["ts"] = time.time()
    tmp = MET.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
    tmp.replace(MET)


def call(method: str, params, *, channel: str = "RPC_MISC", retries: int = 20):
    last = None
    nkeys = 0
    if method == "getMultipleAccounts" and isinstance(params, list) and params:
        nkeys = len(params[0]) if isinstance(params[0], list) else 0
        if nkeys > MAX_KEYS:
            raise ValueError("getMultipleAccounts exceeds 100 keys")
    for attempt in range(max(1, retries)):
        acquire()
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
        req = urllib.request.Request(
            rpc_url(), data=body, headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=45) as r:
                obj = json.loads(r.read().decode())
            if obj.get("error"):
                err = obj["error"]
                msg = str(err)
                if "429" in msg or "rate" in msg.lower() or err.get("code") in (-32429, 429):
                    raise RateLimit(msg)
                raise RuntimeError(err)
            _note(channel, method, True, nkeys)
            return obj["result"]
        except (urllib.error.HTTPError, urllib.error.URLError, RateLimit) as ex:
            last = ex
            code = getattr(ex, "code", 429 if isinstance(ex, RateLimit) else 0)
            if code == 429 or isinstance(ex, RateLimit):
                wait = min(8.0, (0.4 * (2 ** min(attempt, 5))) + random.random() * 0.25)
                print(f"RPC-SCHED  429 ch={channel} {method} backoff={wait:.2f}s", flush=True)
                time.sleep(wait)
                continue
            time.sleep(min(4.0, 0.3 * (attempt + 1)))
        except Exception as ex:
            last = ex
            time.sleep(min(4.0, 0.3 * (attempt + 1)))
    _note(channel, method, False, nkeys)
    raise last


def get_multiple_accounts(keys: list[str], *, channel: str = "RPC_MISC",
                          commitment: str | None = None,
                          min_context_slot: int | None = None,
                          retries: int = 20) -> list[dict | None]:
    import base64
    config = {"encoding": "base64"}
    if commitment is not None:
        if commitment not in ("processed", "confirmed", "finalized"):
            raise ValueError("invalid commitment")
        config["commitment"] = commitment
    if min_context_slot is not None:
        if type(min_context_slot) is not int or min_context_slot < 0:
            raise ValueError("invalid min_context_slot")
        config["minContextSlot"] = min_context_slot
    out: list[dict | None] = []
    for i in range(0, len(keys), MAX_KEYS):
        chunk = keys[i:i + MAX_KEYS]
        res = call("getMultipleAccounts", [chunk, config], channel=channel, retries=retries)
        ctx_slot = res["context"]["slot"]
        if min_context_slot is not None and ctx_slot < min_context_slot:
            raise RuntimeError("RPC context below requested minimum")
        if len(res["value"]) != len(chunk):
            raise RuntimeError("RPC account count mismatch")
        for acc in res["value"]:
            if acc is None:
                out.append(None)
                continue
            out.append({
                "slot": ctx_slot,
                "commitment": commitment,
                "owner": acc["owner"],
                "data": base64.b64decode(acc["data"][0]),
                "lamports": acc["lamports"],
                "executable": acc.get("executable"),
            })
    return out
