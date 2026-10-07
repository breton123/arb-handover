#!/usr/bin/env python3
"""Summarize first-DIRTY events and Pump ix families. No RPC."""
from __future__ import annotations

import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

KNOWN = {
    "33e685a4017f83ad": "sell",
    "c62e1552b4d9e870": "buy_exact_quote_in",
    "66063d1201daebea": "exact_out_buy",
}


def main() -> int:
    dirty_path = Path(sys.argv[1] if len(sys.argv) > 1 else "run/first_dirty.jsonl")
    fam_path = Path(sys.argv[2] if len(sys.argv) > 2 else "run/pump_fam.jsonl")
    if not dirty_path.exists():
        print("missing", dirty_path, file=sys.stderr)
        return 2
    by_cause: Counter[str] = Counter()
    by_cause_disc: dict[str, Counter[str]] = defaultdict(Counter)
    examples: dict[tuple[str, str], dict] = {}
    n = 0
    for line in dirty_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        n += 1
        cause = row.get("cause") or "OTHER"
        disc = row.get("disc_hex") or ""
        by_cause[cause] += 1
        by_cause_disc[cause][disc] += 1
        key = (cause, disc)
        if key not in examples:
            examples[key] = row
    print("first DIRTY events:", n)
    print()
    print(f"{'DIRTY reason':<32} pools")
    print("-" * 40)
    for cause, c in by_cause.most_common():
        print(f"{cause:<32} {c}")
    print()
    for cause, discs in by_cause_disc.items():
        print(f"## {cause}")
        for disc, c in discs.most_common():
            ex = examples[(cause, disc)]
            kind = KNOWN.get(disc, "unknown")
            print(
                f"  disc={disc or '(none)'}  kind={kind}  pools={c}  "
                f"dlen={ex.get('dlen')}  nacc={ex.get('nacc')}  "
                f"where={ex.get('where')}  "
                f"ix={ex.get('ix_index')}  acc_i={ex.get('account_index')}  "
                f"w={ex.get('writable')}  "
                f"row={ex.get('compact_row_present')}/"
                f"{ex.get('compact_row_exact')}  "
                f"acct={str(ex.get('account_pubkey') or '')[:16]}  "
                f"sig={ex.get('sig_hex', '')[:16]}...  "
                f"slot={ex.get('slot')}"
            )
        print()
    if fam_path.exists():
        fams = []
        for line in fam_path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                fams.append(json.loads(line))
        fams.sort(key=lambda r: int(r.get("n") or 0), reverse=True)
        total = sum(int(r.get("n") or 0) for r in fams)
        print("Pump ix families (LUT-resolved, all pools), n=", total)
        print()
        for r in fams:
            disc = r.get("disc_hex") or ""
            kind = KNOWN.get(disc, "unknown")
            effect = "MUTATE_QUOTE" if disc in KNOWN else "UNKNOWN"
            print(
                f"  {kind:<22} {disc}  n={r.get('n')}  "
                f"dlen={r.get('dlen')} nacc={r.get('nacc')}  "
                f"quote_effect={effect}  sig={str(r.get('sig_hex', ''))[:16]}..."
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
