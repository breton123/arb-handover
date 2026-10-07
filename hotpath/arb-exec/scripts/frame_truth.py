#!/usr/bin/env python3
"""Join CORE-010 framing classes with trigger outcomes. No send."""
from __future__ import annotations

import json
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

OUTCOMES = Path("/home/louis/arb-cap/oneshot/trigger_outcomes.jsonl")
SIGS = Path("/tmp/entry_truth_sigs.hex")
RAW = Path("/home/louis/arb-cap/oneshot/frame_truth.jsonl")
TABLE = Path("/home/louis/arb-cap/oneshot/FRAME_TRUTH.json")
BIN = Path("/home/louis/arb-feed/build/frame_truth")
CAP_DIR = Path("/home/louis/captures/orbitflare")
HORIZONS = ("t0", "t0_prefix", "t1", "t2", "fec", "rs", "slot")


def load_outcomes(limit: int = 400) -> list[dict]:
    rows = []
    if not OUTCOMES.exists():
        return rows
    for line in OUTCOMES.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("sig") and rec.get("outcome"):
            rows.append(rec)
    return rows[-limit:]


def main() -> int:
    rows = load_outcomes(400)
    if not rows:
        print("no outcomes", flush=True)
        return 1
    sys.path.insert(0, "/home/louis/arb-cap/paper004")
    import paper004 as p4

    hexes = []
    by_hex = {}
    for rec in rows:
        try:
            hx = p4.b58decode(rec["sig"]).hex()
        except Exception:
            continue
        hexes.append(hx)
        by_hex[hx] = rec
    SIGS.write_text("\n".join(hexes) + "\n", encoding="utf-8")
    print(f"CORE-010  n={len(hexes)}  {BIN}", flush=True)
    proc = subprocess.run(
        [str(BIN), "--dir", str(CAP_DIR), "--sigs", str(SIGS), "--newest", "20"],
        check=False, capture_output=True, text=True,
    )
    sys.stderr.write(proc.stderr[-2500:] if proc.stderr else "")
    if proc.returncode != 0:
        print("frame_truth failed", proc.returncode, flush=True)
        return 1
    RAW.write_text(proc.stdout, encoding="utf-8")
    n_commit = sum(1 for r in rows if r["outcome"] == "landed-success")
    horizons = {}
    for hz in HORIZONS:
        counts = Counter()
        landed = defaultdict(Counter)
        us = []
        for line in proc.stdout.splitlines():
            if not line.startswith("{"):
                continue
            row = json.loads(line)
            oc = (by_hex.get(row["sig_hex"]) or {}).get("outcome")
            cell = row.get(hz) or {"have": 0, "class": "incomplete", "us": 0}
            klass = cell.get("class") or "incomplete"
            if not cell.get("have"):
                klass = "incomplete"
            counts[klass] += 1
            if oc:
                landed[klass][oc] += 1
            if cell.get("have") and klass == "framed":
                us.append(int(cell.get("us") or 0))
        us.sort()

        def pct(xs, p):
            if not xs:
                return None
            return xs[int(p * (len(xs) - 1))]

        table = []
        tp = fp = fn = 0
        for klass in ("invalid", "incomplete", "framed"):
            n = counts[klass]
            ok = landed[klass].get("landed-success", 0)
            gone = landed[klass].get("disappeared", 0)
            fail = landed[klass].get("landed-failed", 0)
            commit = ok + fail
            table.append({
                "class": klass,
                "n": n,
                "landed_success": ok,
                "disappeared": gone,
                "commit_pct": (100.0 * commit / n) if n else None,
            })
            if klass == "framed":
                tp += ok
                fp += gone + fail
            else:
                fn += ok
        prec = (100.0 * tp / (tp + fp)) if (tp + fp) else None
        reca = (100.0 * tp / n_commit) if n_commit else None
        horizons[hz] = {
            "table": table,
            "precision_if_send_framed": prec,
            "recall_of_landed": reca,
            "framed_us": {
                "n": len(us),
                "p50": pct(us, 0.50),
                "p90": pct(us, 0.90),
                "max": us[-1] if us else None,
            },
        }
    out = {"n": len(hexes), "landed": n_commit, "horizons": horizons}
    TABLE.write_text(json.dumps(out, indent=2) + "\n")
    print(json.dumps(out, indent=2), flush=True)
    print("WROTE", TABLE, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
