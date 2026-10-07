#!/usr/bin/env python3
"""Off-path ALT control plane. No hot-path RPC.

Collects Address Lookup Table pubkeys from:
  - late-121 trigger bytes
  - live alt_miss.jsonl
Fetches getAddressLookupTable and writes:
  alt_cache.json
  alt_cache.bin  (ALT1, consumed by paper_orbit)
"""
from __future__ import annotations

import json
import struct
import sys
import time
import urllib.request
from pathlib import Path

from b58 import b58decode, b58encode
from hist_funnel011 import attach_hex, load_races, parse_any
from rpc_url import rpc_url

OUT_DIR = Path(r"c:\Users\louis\Desktop\TheMoneyMaker") / "arb-cap" / "trigger011"
LIVE_MISS = Path("/home/louis/captures/trigger011/alt_miss.jsonl")
JSON_OUT = OUT_DIR / "alt_cache.json"
BIN_OUT = OUT_DIR / "alt_cache.bin"


def rpc(url: str, method: str, params):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode())


def fetch_one(url: str, pk: str) -> list[str] | None:
    doc = rpc(url, "getAddressLookupTable", [pk])
    val = ((doc.get("result") or {}).get("value") or {})
    addrs = val.get("addresses")
    if addrs:
        return list(addrs)
    info = rpc(url, "getAccountInfo", [pk, {"encoding": "base64"}])
    raw_b64 = ((info.get("result") or {}).get("value") or {}).get("data")
    if not raw_b64:
        return None
    import base64
    raw = base64.b64decode(raw_b64[0] if isinstance(raw_b64, list) else raw_b64)
    # AddressLookupTableAccount: 56-byte header-ish then 32-byte addrs.
    # deactivation(8)+last_ext(8)+start(1)+option+maybe auth(32) after 4-byte type.
    if len(raw) < 56:
        return None
    off = 56
    if raw[22] == 1:
        off = 56
    addrs = []
    while off + 32 <= len(raw):
        addrs.append(b58encode(raw[off:off + 32]))
        off += 32
    return addrs or None


def collect_luts() -> set[str]:
    want: set[str] = set()
    rows = load_races()
    attach_hex(rows)
    for r in rows:
        hx = r.get("trigger_tx_hex") or ""
        if not hx:
            continue
        parsed = parse_any(bytes.fromhex(hx))
        for lut in parsed.get("luts") or []:
            want.add(lut)
    if LIVE_MISS.exists():
        for line in LIVE_MISS.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            hx = rec.get("alt_hex") or ""
            if len(hx) == 64:
                want.add(b58encode(bytes.fromhex(hx)))
    return want


def write_bin(cache: dict[str, list[str]], path: Path) -> None:
    recs = []
    for pk, addrs in cache.items():
        recs.append((b58decode(pk), [b58decode(a) for a in addrs[:256]]))
    raw = struct.pack("<I", 0x31544C41) + struct.pack("<I", len(recs))
    for pk, addrs in recs:
        raw += pk[:32].rjust(32, b"\x00")
        raw += struct.pack("<H", len(addrs))
        for a in addrs:
            raw += a[:32].rjust(32, b"\x00")
    path.write_bytes(raw)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    url = rpc_url()
    existing = {}
    if JSON_OUT.exists():
        existing = json.loads(JSON_OUT.read_text(encoding="utf-8"))
    want = collect_luts()
    print(f"unique ALTs requested={len(want)} already={len(existing)}")
    ok = 0
    fail = 0
    t0 = time.time()
    for i, pk in enumerate(sorted(want)):
        if pk in existing and existing[pk]:
            ok += 1
            continue
        try:
            addrs = fetch_one(url, pk)
        except Exception:
            addrs = None
        if addrs:
            existing[pk] = addrs
            ok += 1
        else:
            fail += 1
        if (i + 1) % 10 == 0:
            print(f"  progress {i+1}/{len(want)} ok={ok} fail={fail}")
        time.sleep(0.05)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    JSON_OUT.write_text(json.dumps(existing, indent=2), encoding="utf-8")
    write_bin(existing, BIN_OUT)
    print(f"wrote {JSON_OUT.name} n={len(existing)} ok={ok} fail={fail} "
          f"lat_s={time.time()-t0:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
