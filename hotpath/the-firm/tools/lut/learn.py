#!/usr/bin/env python3
"""Off-path ALT fetch: getAccountInfo only. Does not print tokens."""
from __future__ import annotations

import json
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "anchor"))

from envload import load_env, rpc_url  # noqa: E402
from account import ALT_OWNER, parse_account  # noqa: E402
from pda import b58decode, b58encode  # noqa: E402


def _rpc(url: str, method: str, params: list):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
    req = urllib.request.Request(
        url, data=body.encode(), headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            payload = json.loads(resp.read().decode())
    except Exception:
        return None
    if payload.get("error"):
        return None
    return payload.get("result")


def fetch_table(url: str, b58: str) -> dict | None:
    result = _rpc(
        url,
        "getAccountInfo",
        [b58, {"encoding": "base64", "commitment": "finalized"}],
    )
    if result is None:
        fetch_table.last = "rpc"
        return None
    if not result.get("value"):
        fetch_table.last = "no_account"
        return None
    val = result["value"]
    owner = val.get("owner") or ""
    if owner != ALT_OWNER:
        fetch_table.last = "owner"
        return None
    blob, enc = val["data"][0], val["data"][1]
    if enc != "base64":
        fetch_table.last = "enc"
        return None
    import base64

    raw = base64.b64decode(blob)
    parsed = parse_account(raw)
    if parsed is None:
        fetch_table.last = "parse_n%d_t%d" % (len(raw), int.from_bytes(raw[:4], "little") if len(raw) >= 4 else -1)
        return None
    parsed["slot"] = int(result["context"]["slot"])
    parsed["table_b58"] = b58
    fetch_table.last = "ok"
    return parsed


fetch_table.last = ""


def row_json(parsed: dict) -> str:
    key = b58decode(parsed["table_b58"])
    addrs = [a.hex() for a in parsed["addrs"]]
    return json.dumps(
        {
            "key_hex": key.hex(),
            "addrs_hex": addrs,
            "deactivation_slot": parsed["deactivation_slot"],
            "last_extended_slot": parsed["last_extended_slot"],
            "start_index": parsed["start_index"],
            "slot": parsed["slot"],
            "commitment": "finalized",
        }
    )


SKIP_FAIL: set[str] = set()


def learn_once(miss_path: Path, learned_path: Path, url: str) -> int:
    seen = set()
    if learned_path.exists():
        for line in learned_path.read_text(encoding="utf-8").splitlines():
            try:
                seen.add(json.loads(line).get("key_hex", ""))
            except json.JSONDecodeError:
                continue
    n = 0
    attempts = 0
    none = 0
    if not miss_path.exists():
        return 0
    for line in miss_path.read_text(encoding="utf-8").splitlines():
        if attempts >= 16:
            break
        try:
            hx = json.loads(line).get("table_hex", "")
        except json.JSONDecodeError:
            continue
        if not hx or hx in seen or hx in SKIP_FAIL:
            continue
        attempts += 1
        try:
            b58 = b58encode(bytes.fromhex(hx))
        except ValueError:
            SKIP_FAIL.add(hx)
            continue
        parsed = fetch_table(url, b58)
        if parsed is None:
            none += 1
            if fetch_table.last in ("no_account", "owner", "enc") or fetch_table.last.startswith("parse"):
                SKIP_FAIL.add(hx)
            continue
        with learned_path.open("a", encoding="utf-8") as fp:
            fp.write(row_json(parsed) + "\n")
        seen.add(hx)
        n += 1
    if attempts:
        print("lut_fetch tried", attempts, "ok", n, "none", none,
              "last", fetch_table.last, file=sys.stderr)
    return n


def main() -> int:
    load_env()
    url = rpc_url()
    if not url:
        print("missing RPC URL", file=sys.stderr)
        return 2
    miss = Path(sys.argv[1] if len(sys.argv) > 1 else "run/lut_miss.jsonl")
    learned = Path(sys.argv[2] if len(sys.argv) > 2 else "run/lut_learned.jsonl")
    loop = "--loop" in sys.argv
    while True:
        n = learn_once(miss, learned, url)
        if n:
            print("learned", n, file=sys.stderr)
        if not loop:
            break
        time.sleep(0.5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
