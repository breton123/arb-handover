#!/usr/bin/env python3
"""TRIGGER-011 permanent late-121 detector regression.

Account-centric. Does not require a static DEX program id.
Does not invent S'. ALT contents are not in this corpus → ALT_MISS
is a classified remainder, not an unaccounted miss.
"""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(r"c:\Users\louis\Desktop\TheMoneyMaker")
RACES = ROOT / "arb-cap" / "cap005_late" / "races.jsonl"
PAIR_FILES = [
    ROOT / "arb-cap" / "cap004_s25" / "pairs.jsonl",
    ROOT / "arb-cap" / "cap004" / "pairs.jsonl",
    ROOT / "arb-cap" / "shred_v1_fat" / "pairs.jsonl",
]
UNIV = ROOT / "arb-cap" / "regress" / "liveuniv_now.json"
OUT = ROOT / "arb-cap" / "trigger011" / "hist_funnel011.jsonl"
ALT_JSON = ROOT / "arb-cap" / "trigger011" / "alt_cache.json"
SOL = "So11111111111111111111111111111111111111112"

PROG_DLMM = bytes([
    0x04, 0xE9, 0xE1, 0x2F, 0xBC, 0x84, 0xE8, 0x26,
    0xC9, 0x32, 0xCC, 0xE9, 0xE2, 0x64, 0x0C, 0xCE,
    0x15, 0x59, 0x0C, 0x1C, 0x62, 0x73, 0xB0, 0x92,
    0x57, 0x08, 0xBA, 0x3B, 0x85, 0x20, 0xB0, 0xBC,
])
PROG_PUMP = bytes([
    0x0C, 0x14, 0xDE, 0xFC, 0x82, 0x5E, 0xC6, 0x76,
    0x94, 0x25, 0x08, 0x18, 0xBB, 0x65, 0x40, 0x65,
    0xF4, 0x29, 0x8D, 0x31, 0x56, 0xD5, 0x71, 0xB4,
    0xD4, 0xF8, 0x09, 0x0C, 0x18, 0xE9, 0xA8, 0x63,
])
DLMM_SWAP2 = bytes([0x41, 0x4B, 0x3F, 0x4C, 0xEB, 0x5B, 0x5B, 0x88])
DLMM_SWAP1 = bytes([0xF8, 0xC6, 0x9E, 0x91, 0xE1, 0x75, 0x87, 0xC8])
PUMP_SELL = bytes([0x33, 0xE6, 0x85, 0xA4, 0x01, 0x7F, 0x83, 0xAD])
PUMP_BUY_EQ = bytes([0xC6, 0x2E, 0x15, 0x52, 0xB4, 0xD9, 0xE8, 0x70])
TX_SIG_MAX, TX_KEY_MAX, TX_INSTR_MAX = 12, 64, 64


def cu16(b: bytes, off: int):
    if off >= len(b):
        return None
    b0 = b[off]
    if (b0 & 0x80) == 0:
        return b0, off + 1
    if off + 1 >= len(b):
        return None
    b1 = b[off + 1]
    if (b1 & 0x80) == 0:
        if b1 == 0:
            return "bad"
        return (b0 & 0x7F) | (b1 << 7), off + 2
    if off + 2 >= len(b):
        return None
    b2 = b[off + 2]
    if (b2 & 0xFC) != 0 or b2 == 0:
        return "bad"
    return (b0 & 0x7F) | ((b1 & 0x7F) << 7) | (b2 << 14), off + 3


def b58(raw: bytes) -> str:
    alph = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n = int.from_bytes(raw, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = alph[r] + out
    pad = 0
    for byte in raw:
        if byte == 0:
            pad += 1
        else:
            break
    return "1" * pad + (out or "1")


def parse_any(raw: bytes) -> dict:
    """Generic CORE-010 structure. v0 indexes may exceed static nkeys."""
    out = {
        "klass": "absent",
        "why": "no bytes",
        "static_keys": [],
        "luts": [],
        "lut_tables": [],
        "ix": [],
        "dex_static": False,
        "direct_static": False,
        "nkeys": 0,
        "versioned": False,
    }
    if not raw:
        return out
    got = cu16(raw, 0)
    if got is None:
        out["klass"] = "incomplete"
        out["why"] = "truncated_nsig"
        return out
    if got == "bad":
        out["klass"] = "invalid"
        out["why"] = "bad_nsig"
        return out
    nsig, off = got
    if nsig < 1 or nsig > TX_SIG_MAX:
        out["klass"] = "invalid"
        out["why"] = "nsig_range"
        return out
    if off + nsig * 64 > len(raw):
        out["klass"] = "incomplete"
        out["why"] = "truncated_sigs"
        return out
    off += nsig * 64
    if off >= len(raw):
        out["klass"] = "incomplete"
        out["why"] = "truncated_hdr"
        return out
    b0 = raw[off]
    off += 1
    versioned = False
    if b0 & 0x80:
        if (b0 & 0x7F) != 0:
            out["klass"] = "invalid"
            out["why"] = "bad_version"
            return out
        versioned = True
        if off >= len(raw) or raw[off] != nsig:
            out["klass"] = "invalid"
            out["why"] = "hdr_nsig"
            return out
        off += 1
    elif b0 != nsig:
        out["klass"] = "invalid"
        out["why"] = "hdr_nsig"
        return out
    if off + 2 > len(raw):
        out["klass"] = "incomplete"
        out["why"] = "truncated_hdr"
        return out
    off += 2
    got = cu16(raw, off)
    if got is None:
        out["klass"] = "incomplete"
        out["why"] = "truncated_nkeys"
        return out
    if got == "bad":
        out["klass"] = "invalid"
        out["why"] = "bad_nkeys"
        return out
    nkeys, off = got
    out["nkeys"] = nkeys
    out["versioned"] = versioned
    if nkeys < nsig or nkeys > TX_KEY_MAX:
        out["klass"] = "invalid"
        out["why"] = "nkeys_range"
        return out
    if off + nkeys * 32 + 32 > len(raw):
        out["klass"] = "incomplete"
        out["why"] = "truncated_keys"
        return out
    keys = [raw[off + i * 32: off + (i + 1) * 32] for i in range(nkeys)]
    out["static_keys"] = [b58(k) for k in keys]
    out["dex_static"] = any(k in (PROG_DLMM, PROG_PUMP) for k in keys)
    off += nkeys * 32 + 32
    got = cu16(raw, off)
    if got is None:
        out["klass"] = "incomplete"
        out["why"] = "truncated_ix"
        return out
    if got == "bad":
        out["klass"] = "invalid"
        out["why"] = "bad_ix"
        return out
    ninstr, off = got
    if ninstr == 0 or ninstr > TX_INSTR_MAX:
        out["klass"] = "invalid"
        out["why"] = "ninstr"
        return out
    for _ in range(ninstr):
        if off >= len(raw):
            out["klass"] = "incomplete"
            out["why"] = "truncated_ix"
            return out
        prog_idx = raw[off]
        off += 1
        if not versioned and prog_idx >= nkeys:
            out["klass"] = "invalid"
            out["why"] = "prog_idx"
            return out
        if prog_idx < nkeys and keys[prog_idx] in (PROG_DLMM, PROG_PUMP):
            out["direct_static"] = True
        got = cu16(raw, off)
        if got is None or got == "bad":
            out["klass"] = "incomplete" if got is None else "invalid"
            out["why"] = "ix_acc"
            return out
        nacct, off = got
        if off + nacct > len(raw):
            out["klass"] = "incomplete"
            out["why"] = "truncated_acc"
            return out
        acc = list(raw[off:off + nacct])
        off += nacct
        got = cu16(raw, off)
        if got is None or got == "bad":
            out["klass"] = "incomplete" if got is None else "invalid"
            out["why"] = "ix_data"
            return out
        dlen, off = got
        if off + dlen > len(raw):
            out["klass"] = "incomplete"
            out["why"] = "truncated_data"
            return out
        data = raw[off:off + dlen]
        off += dlen
        out["ix"].append({"prog": prog_idx, "acc": acc, "data": data.hex()})
    if versioned:
        got = cu16(raw, off)
        if got is None or got == "bad":
            out["klass"] = "incomplete" if got is None else "invalid"
            out["why"] = "lut"
            return out
        nlut, off = got
        if nlut > 32:
            out["klass"] = "invalid"
            out["why"] = "nlut"
            return out
        for _ in range(nlut):
            if off + 32 > len(raw):
                out["klass"] = "incomplete"
                out["why"] = "truncated_alt"
                return out
            lut_pk = b58(raw[off:off + 32])
            out["luts"].append(lut_pk)
            off += 32
            idxs = []
            for _w in range(2):
                got = cu16(raw, off)
                if got is None or got == "bad":
                    out["klass"] = "incomplete" if got is None else "invalid"
                    out["why"] = "alt_idx"
                    return out
                nw, off = got
                if off + nw > len(raw):
                    out["klass"] = "incomplete"
                    out["why"] = "truncated_alt_idx"
                    return out
                idxs.append(list(raw[off:off + nw]))
                off += nw
            out["lut_tables"].append({"pk": lut_pk, "wr": idxs[0], "ro": idxs[1]})
    out["klass"] = "framed"
    out["why"] = "generic"
    return out


def load_univ(path: Path) -> dict:
    doc = json.loads(path.read_text(encoding="utf-8"))
    watch = {}
    for p in doc.get("pools") or []:
        pk = p.get("pubkey")
        if not pk:
            continue
        watch[pk] = {"role": "pool", "proto": p.get("proto"), "idx": p.get("idx")}
        for k in ("vault_x", "vault_y", "vx", "vy"):
            v = p.get(k)
            if v:
                watch[v] = {"role": "vault", "proto": p.get("proto"), "idx": p.get("idx")}
    return watch


def attach_hex(rows: list[dict]) -> None:
    want = {r["sig"] for r in rows}
    found: dict[str, dict] = {}
    key = '"mriya_sig":"'
    for path in PAIR_FILES:
        if not path.exists() or len(found) == len(want):
            continue
        with path.open(encoding="utf-8") as f:
            for line in f:
                i = line.find(key)
                if i < 0:
                    continue
                j = line.find('"', i + len(key))
                sig = line[i + len(key): j]
                if sig not in want or sig in found:
                    continue
                found[sig] = json.loads(line)
    for r in rows:
        p = found.get(r["sig"]) or {}
        r["trigger_tx_hex"] = p.get("trigger_tx_hex") or ""
        r["pair_hit"] = bool(p)


def load_races() -> list[dict]:
    rows = []
    for line in RACES.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        if r.get("supported") and r.get("route") == "Meteora DLMM → Pump Swap":
            rows.append(r)
    return rows


def load_alts() -> dict:
    if not ALT_JSON.exists():
        return {}
    return json.loads(ALT_JSON.read_text(encoding="utf-8"))


def resolve_loaded(parsed: dict, alts: dict) -> tuple[list[str], str]:
    keys = list(parsed.get("static_keys") or [])
    if not parsed.get("versioned"):
        return keys, "legacy"
    miss = []
    for tab in parsed.get("lut_tables") or []:
        addrs = alts.get(tab["pk"])
        if not addrs:
            miss.append(tab["pk"])
            continue
        for idx in tab.get("wr") or []:
            if idx < len(addrs):
                keys.append(addrs[idx])
        for idx in tab.get("ro") or []:
            if idx < len(addrs):
                keys.append(addrs[idx])
    if miss:
        return keys, "alt_miss"
    return keys, "resolved"


def decode_direct(parsed: dict, loaded: list[str]) -> dict | None:
    for ix in parsed.get("ix") or []:
        pi = ix["prog"]
        if pi >= len(loaded):
            continue
        prog = loaded[pi]
        data = bytes.fromhex(ix["data"])
        if len(data) < 24:
            continue
        amt = int.from_bytes(data[8:16], "little")
        if amt == 0:
            continue
        acc = ix["acc"]
        pool = loaded[acc[0]] if acc and acc[0] < len(loaded) else ""
        if prog == b58(PROG_PUMP) and data[:8] in (PUMP_SELL, PUMP_BUY_EQ):
            return {"proto": "pump", "amount_in": amt, "pool": pool, "variant": data[:8].hex()}
        if prog == b58(PROG_DLMM) and data[:8] in (DLMM_SWAP2, DLMM_SWAP1):
            return {"proto": "dlmm", "amount_in": amt, "pool": pool, "variant": data[:8].hex()}
    return None


def classify_row(r: dict, watch: dict, alts: dict) -> dict:
    raw = bytes.fromhex(r["trigger_tx_hex"]) if r.get("trigger_tx_hex") else b""
    parsed = parse_any(raw)
    loaded, resolve = resolve_loaded(parsed, alts)
    watch_hit = [k for k in loaded if k in watch]
    need_alt = bool(parsed.get("luts"))
    direct = decode_direct(parsed, loaded) if parsed["klass"] == "framed" else None
    if not r.get("trigger_tx_hex"):
        stage = "no_bytes"
    elif parsed["klass"] != "framed":
        stage = parsed["klass"]
    elif direct:
        stage = "direct_exact"
    elif watch_hit:
        stage = "relevant_unknown"
    elif need_alt and resolve == "alt_miss":
        stage = "alt_miss"
    else:
        stage = "unrecoverable"
    return {
        "sig": r["sig"],
        "profit": r.get("profit") or 0,
        "trigger_recovered": bool(r.get("trigger_tx_hex")),
        "frame": parsed["klass"],
        "frame_why": parsed["why"],
        "versioned": parsed.get("versioned"),
        "dex_static": parsed.get("dex_static"),
        "direct_static": parsed.get("direct_static"),
        "n_lut": len(parsed.get("luts") or []),
        "resolve": resolve,
        "n_loaded": len(loaded),
        "static_watch": len([k for k in (parsed.get("static_keys") or []) if k in watch]),
        "watch_hit": len(watch_hit),
        "direct": direct,
        "stage": stage,
    }


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    watch = load_univ(UNIV)
    alts = load_alts()
    rows = load_races()
    attach_hex(rows)
    scored = [classify_row(r, watch, alts) for r in rows]
    n = len(scored)
    usd = sum(r["profit"] for r in scored)

    def c(pred):
        xs = [r for r in scored if pred(r)]
        return len(xs), sum(r["profit"] for r in xs)

    print(f"TRIGGER-011 LATE-121  n={n}  ${usd:.1f}")
    print(f"{'stage':<42} {'N':>5} {'$':>10}")
    steps = [
        ("KNOWN REAL LATE ARBS", lambda r: True),
        ("raw trigger available", lambda r: r["trigger_recovered"]),
        ("generic FRAMED", lambda r: r["frame"] == "framed"),
        ("ALT referenced (needs cache)", lambda r: r["n_lut"] > 0),
        ("ALT resolved", lambda r: r.get("resolve") in ("legacy", "resolved") and r["frame"] == "framed"),
        ("watched-account relevant", lambda r: r.get("watch_hit", 0) > 0),
        ("direct exact N", lambda r: r["stage"] == "direct_exact"),
        ("router exact/predictable N", lambda r: False),
        ("RELEVANT_UNKNOWN (no exact N)", lambda r: r["stage"] == "relevant_unknown"),
        ("ALT_MISS (no fake S')", lambda r: r["stage"] == "alt_miss"),
        ("unrecoverable/bad capture", lambda r: r["stage"] in ("unrecoverable", "invalid", "incomplete", "no_bytes", "absent")),
    ]
    for name, pred in steps:
        nn, dollars = c(pred)
        print(f"{name:<42} {nn:5} {dollars:10.1f}")
    print("router exact/predictable N                  0         0.0  (no invented decoder)")
    print("state/S' for unknown                        0         0.0  fail closed")
    print("drop reasons:", dict(Counter(r["frame_why"] for r in scored)))
    print("stages:", dict(Counter(r["stage"] for r in scored)))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("".join(json.dumps(x) + "\n" for x in scored), encoding="utf-8")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
