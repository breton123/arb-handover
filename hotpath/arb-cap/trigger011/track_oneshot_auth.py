#!/usr/bin/env python3
"""Explain AUTH status for the three frozen oneshot pairs. Read-only."""
from __future__ import annotations

import json
import os
from collections import Counter
from pathlib import Path

CAP = Path(os.environ.get("STATE008_CAP") or "/home/louis/captures/state008")
ONESHOT = {
    "9xiLuqDSvN1pgyHSHCeRbrKFTe1DWbPAMJRm2usYgdaX": "dlmm",
    "2i2iULr7UwK1SDRB17T5FUtiMnFQL7fyy7rh69brZihc": "pump",
    "8eLx26sto5xBSg8r1ksidtkmQyHzV2JUrQU97qmKw1Tq": "dlmm",
    "8R4DDi9X3RBLr876hhRJCv4Gvsy8d9SkABgb8YiHapo9": "pump",
    "E27r15wB77JfF1hFkfeqoP2ZyZHKMWKgZ9Aix2cdPAuq": "dlmm",
    "ASgoadVEDL8zJn6KLUiMSh9m8x2yYAEZPsUSpvxHj3LY": "pump",
}


def main() -> int:
    pools = json.loads((CAP / "POOLS.json").read_text(encoding="utf-8"))
    rows = pools if isinstance(pools, list) else (pools.get("pools") or pools.get("rows") or [])
    counts = Counter()
    by_pk = {}
    for rec in rows:
        if not isinstance(rec, dict):
            continue
        st = rec.get("status") or rec.get("st") or rec.get("state")
        counts[str(st)] += 1
        pk = rec.get("pubkey") or rec.get("pool")
        if pk in ONESHOT:
            by_pk[pk] = rec
    print(json.dumps({
        "cap": str(CAP),
        "ready": (CAP / "READY").exists(),
        "n": len(rows),
        "status_counts": dict(counts),
        "oneshot": {
            pk: {
                "role": role,
                "found": pk in by_pk,
                "status": (by_pk.get(pk) or {}).get("status") or (by_pk.get(pk) or {}).get("st"),
                "idx": (by_pk.get(pk) or {}).get("idx"),
                "kind": (by_pk.get(pk) or {}).get("kind"),
                "n_required": (by_pk.get(pk) or {}).get("n_required"),
                "n_have": (by_pk.get(pk) or {}).get("n_have"),
                "active_id": (by_pk.get(pk) or {}).get("active_id"),
                "gen": (by_pk.get(pk) or {}).get("gen"),
            }
            for pk, role in ONESHOT.items()
        },
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
