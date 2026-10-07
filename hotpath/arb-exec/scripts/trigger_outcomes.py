#!/usr/bin/env python3
"""Follow opp_synced and record N outcomes. SEND=0. No funded fire."""
from __future__ import annotations

import json
import os
import time
from collections import Counter
from pathlib import Path

import sys

sys.path.insert(0, "/home/louis/arb-cap")
sys.path.insert(0, "/home/louis/arb-cap/paper004")
import live001 as live
import paper004 as p4

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
OUT = Path("/home/louis/arb-cap/oneshot/trigger_outcomes.jsonl")
SUM = Path("/home/louis/arb-cap/oneshot/TRIGGER_TABLE.json")
WAIT_S = 20.0


def classify(sig: str) -> str:
    try:
        tx = p4.fetch_tx(sig)
    except Exception:
        return "disappeared"
    if not tx:
        return "disappeared"
    if (tx.get("meta") or {}).get("err"):
        return "landed-failed"
    return "landed-success"


def already() -> set[str]:
    have: set[str] = set()
    if not OUT.exists():
        return have
    with OUT.open("r", encoding="utf-8") as f:
        for line in f:
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if row.get("sig"):
                have.add(row["sig"])
    return have


def emit(sig: str, rec: dict, kind: str, counts: Counter) -> None:
    counts[kind] += 1
    row = {
        "ts": time.time(),
        "outcome": kind,
        "sig": sig,
        "pool": rec.get("pool"),
        "n": rec.get("n"),
        "n_ix": rec.get("n_ix"),
        "shred": rec.get("shred"),
        "send_quote": rec.get("send_quote"),
        "arb": rec.get("arb"),
    }
    with OUT.open("a", encoding="utf-8") as o:
        o.write(json.dumps(row) + "\n")
    ntot = sum(counts.values())
    print(f"  {kind} {sig[:12]} n={ntot}", flush=True)
    SUM.write_text(json.dumps({"n": ntot, "counts": dict(counts)}, indent=2) + "\n")


def main() -> int:
    live.load_dotenv()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    pending: list[dict] = []
    counts: Counter[str] = Counter()
    seen = 0
    have = already()
    print("TRIGGER_OUTCOMES  follow opp_synced  SEND=0", flush=True)
    if AUDIT.exists():
        hist_lines = AUDIT.read_text(encoding="utf-8", errors="replace").splitlines()[-400:]
        for line in hist_lines:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") != "opp_synced" or not rec.get("sig_hex"):
                continue
            try:
                sig = p4.b58encode(bytes.fromhex(rec["sig_hex"]))
            except Exception:
                continue
            if sig in have:
                continue
            kind = classify(sig)
            have.add(sig)
            emit(sig, rec, kind, counts)
            seen += 1
        print(f"  backfill done seen={seen} {dict(counts)}", flush=True)
    with AUDIT.open("r", encoding="utf-8") as f:
        f.seek(0, os.SEEK_END)
        while True:
            line = f.readline()
            now = time.time()
            if line:
                line = line.strip()
                if line:
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        rec = None
                    if rec and rec.get("kind") == "opp_synced" and rec.get("sig_hex"):
                        hx = rec["sig_hex"]
                        try:
                            sig = p4.b58encode(bytes.fromhex(hx))
                        except Exception:
                            sig = ""
                        if sig:
                            pending.append({
                                "t0": now,
                                "sig": sig,
                                "rec": rec,
                            })
                            seen += 1
                            print(
                                f"  seen {seen} pool={(rec.get('pool') or '')[:8]} "
                                f"cap_gp={(rec.get('send_quote') or {}).get('cap_gross')}",
                                flush=True,
                            )
            keep = []
            for p in pending:
                if now - p["t0"] < WAIT_S:
                    keep.append(p)
                    continue
                if p["sig"] in have:
                    continue
                kind = classify(p["sig"])
                have.add(p["sig"])
                emit(p["sig"], p["rec"], kind, counts)
            pending = keep
            if not line:
                time.sleep(0.05)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
