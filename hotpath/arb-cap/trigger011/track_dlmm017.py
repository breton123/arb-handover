#!/usr/bin/env python3
"""DLMM-ACTIVEBIN-017 — derive active BinArray PDAs for the three oneshot legs."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "arb-state" / "shyft"))
sys.path.insert(0, str(ROOT / "arb-cap"))
import deps  # noqa: E402
import record_dlmm as d  # noqa: E402

CAP = Path(os.environ.get("STATE008_CAP") or "/home/louis/captures/state008")
ONESHOT = (
    "9xiLuqDSvN1pgyHSHCeRbrKFTe1DWbPAMJRm2usYgdaX",
    "8eLx26sto5xBSg8r1ksidtkmQyHzV2JUrQU97qmKw1Tq",
    "E27r15wB77JfF1hFkfeqoP2ZyZHKMWKgZ9Aix2cdPAuq",
)


def main() -> int:
    doc = json.loads((CAP / "POOLS.json").read_text(encoding="utf-8"))
    rows = {r["pubkey"]: r for r in doc.get("pools") or []}
    out = []
    for pk in ONESHOT:
        rec = rows.get(pk) or {}
        active = rec.get("active_id")
        indexes = d.array_indexes(int(active)) if active is not None else []
        pdas = deps.bin_keys_for_active(pk, int(active)) if active is not None else []
        out.append({
            "dlmm": pk,
            "status": rec.get("status"),
            "n_required": rec.get("n_required"),
            "n_have": rec.get("n_have"),
            "active_id": active,
            "indexes": indexes,
            "bin_pdas": pdas,
        })
    print(json.dumps({
        "ready": (CAP / "READY").exists(),
        "pools": out,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
