#!/usr/bin/env python3
import json
import sys

FAM = {0: "none", 1: "pump_amm", 2: "pump_bond", 3: "dlmm"}

def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pump_five.jsonl"
    for line in open(path, encoding="utf-8"):
        r = json.loads(line)
        print(
            f"slot={r['slot']} entry={r['entry']} tx_i={r['tx_i']} "
            f"v={r['version']} n_ix={r['n_ix']}"
        )
        print(f"  sig={r['sig_hex'][:16]}...")
        for ix in r.get("ix") or []:
            print(
                f"  ix[{ix['i']}] fam={FAM.get(ix['fam'], ix['fam'])} "
                f"prog_i={ix['prog_i']} nacc={ix['nacc']} dlen={ix['dlen']} "
                f"disc={ix['disc']} name={ix['name']}"
            )
            print(f"       prog={ix['prog']}")
        print()


if __name__ == "__main__":
    main()
