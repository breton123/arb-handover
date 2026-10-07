#!/usr/bin/env python3
"""Join entry_truth classes with trigger outcomes. Read-only. No send."""
from __future__ import annotations

import json
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

OUTCOMES = Path("/home/louis/arb-cap/oneshot/trigger_outcomes.jsonl")
SIGS = Path("/tmp/entry_truth_sigs.hex")
RAW = Path("/home/louis/arb-cap/oneshot/entry_truth.jsonl")
TABLE = Path("/home/louis/arb-cap/oneshot/ENTRY_TRUTH.json")
BIN = Path("/home/louis/arb-feed/build/entry_truth")
CAP_DIR = Path("/home/louis/captures/orbitflare")


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
    import paper004 as p4  # noqa: WPS433

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
    print(f"ENTRY-TRUTH  n={len(hexes)}  run {BIN}", flush=True)
    proc = subprocess.run(
        [
            str(BIN),
            "--dir", str(CAP_DIR),
            "--sigs", str(SIGS),
            "--prefix", "orbitflare-",
            "--newest", "20",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    sys.stderr.write(proc.stderr[-2000:] if proc.stderr else "")
    if proc.returncode != 0:
        print("entry_truth failed", proc.returncode, flush=True)
        return 1
    RAW.write_text(proc.stdout, encoding="utf-8")
    counts = Counter()
    landed = defaultdict(Counter)
    dts = []
    cases = []
    for line in proc.stdout.splitlines():
        if not line.startswith("{"):
            continue
        row = json.loads(line)
        hx = row["sig_hex"]
        oc = (by_hex.get(hx) or {}).get("outcome")
        row["outcome"] = oc
        klass = row["class"]
        counts[klass] += 1
        if oc:
            landed[klass][oc] += 1
        if klass == "ENTRY_VALID" and row.get("dt_ns") is not None:
            dts.append(int(row["dt_ns"]))
        cases.append(row)
    dts.sort()

    def pct(p: float) -> int | None:
        if not dts:
            return None
        i = int(p * (len(dts) - 1))
        return dts[i]

    table = []
    for klass in ("FRAGMENT_ONLY", "FALSE_TRIGGER", "ENTRY_VALID", "NO_CAP"):
        n = counts[klass]
        ok = landed[klass].get("landed-success", 0)
        fail = landed[klass].get("landed-failed", 0)
        gone = landed[klass].get("disappeared", 0)
        commit_n = ok + fail
        table.append({
            "class": klass,
            "n": n,
            "landed_success": ok,
            "landed_failed": fail,
            "disappeared": gone,
            "commit_pct": (100.0 * commit_n / n) if n else None,
            "success_given_commit_pct": (100.0 * ok / commit_n) if commit_n else None,
        })
    summary = {
        "n": len(cases),
        "counts": dict(counts),
        "table": table,
        "entry_minus_actionable_ns": {
            "n": len(dts),
            "p50": pct(0.50),
            "p90": pct(0.90),
            "p99": pct(0.99),
            "max": dts[-1] if dts else None,
        },
    }
    TABLE.write_text(json.dumps({"summary": summary, "cases": cases}, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    print("WROTE", TABLE, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
