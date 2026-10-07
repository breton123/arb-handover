#!/usr/bin/env python3
"""A/B first-DIRTY census: old replay vs new, same frozen capture."""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

MISSING = (
    "DEP_MISSING_POOL_ROW",
    "DEP_MISSING_BASE_VAULT_ROW",
    "DEP_MISSING_QUOTE_VAULT_ROW",
    "DEP_MISSING_CONFIG",
    "DEP_MISSING_OTHER_ROW",
)


def load(path: Path) -> tuple[int, Counter[str]]:
    n = 0
    c: Counter[str] = Counter()
    if not path.exists():
        return 0, c
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        n += 1
        c[row.get("cause") or "OTHER"] += 1
    return n, c


def bucket(c: Counter[str], keys: tuple[str, ...]) -> int:
    return sum(c[k] for k in keys)


def main() -> int:
    old_p = Path(sys.argv[1] if len(sys.argv) > 1 else "run/census8h/first_dirty.jsonl")
    new_p = Path(sys.argv[2] if len(sys.argv) > 2 else "run/ab/first_dirty.jsonl")
    n_old, old = load(old_p)
    n_new, new = load(new_p)
    print(f"{'metric':<36} {'old':>8} {'new':>8}")
    print("-" * 54)
    print(f"{'first DIRTY':<36} {n_old:8d} {n_new:8d}")
    print(
        f"{'UNRESOLVED_REQUIRED_ACCOUNT':<36} "
        f"{old['UNRESOLVED_REQUIRED_ACCOUNT']:8d} "
        f"{new['UNRESOLVED_REQUIRED_ACCOUNT']:8d}"
    )
    print(
        f"{'unknown Pump disc':<36} "
        f"{old['UNKNOWN_PUMP_DISC']:8d} {new['UNKNOWN_PUMP_DISC']:8d}"
    )
    print(
        f"{'DEP_UNKNOWN_PUMP_KIND':<36} "
        f"{old['DEP_UNKNOWN_PUMP_KIND']:8d} {new['DEP_UNKNOWN_PUMP_KIND']:8d}"
    )
    print(
        f"{'missing compact rows':<36} "
        f"{bucket(old, MISSING):8d} {bucket(new, MISSING):8d}"
    )
    print(
        f"{'DEP_EXTRA_IX_TOUCHES_STATE':<36} "
        f"{old['DEP_EXTRA_IX_TOUCHES_STATE']:8d} "
        f"{new['DEP_EXTRA_IX_TOUCHES_STATE']:8d}"
    )
    print(
        f"{'DEP_UNKNOWN_CPI_EFFECT':<36} "
        f"{old['DEP_UNKNOWN_CPI_EFFECT']:8d} {new['DEP_UNKNOWN_CPI_EFFECT']:8d}"
    )
    print(
        f"{'APPLY_REJECT':<36} "
        f"{old['APPLY_REJECT']:8d} {new['APPLY_REJECT']:8d}"
    )
    print(
        f"{'LUT_STATIC':<36} {old['LUT_STATIC']:8d} {new['LUT_STATIC']:8d}"
    )
    print()
    print(
        "DEP_NO_QUOTE_EFFECT is apply-success; it does not appear as first-DIRTY."
    )
    print("Compare dashboard 'NO_QUOTE extra (applied)' and EXACT n/272.")
    print()
    vanished = old["UNRESOLVED_REQUIRED_ACCOUNT"] - (
        bucket(new, MISSING)
        + new["DEP_EXTRA_IX_TOUCHES_STATE"]
        + new["DEP_UNKNOWN_CPI_EFFECT"]
        + new["APPLY_REJECT"]
        + new["DEP_UNKNOWN_PUMP_KIND"]
    )
    print(
        "rough: old UNRESOLVED minus new named apply-fails "
        f"(not a perfect 182 split): {vanished}"
    )
    print()
    print("new causes:")
    for k, v in new.most_common():
        print(f"  {k:<36} {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
