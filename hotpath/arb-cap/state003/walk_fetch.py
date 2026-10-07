#!/usr/bin/env python3
"""Control-plane wide bin dump for STATE-003 walk measurement. Not hot path."""
from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

K = 128


def array_indexes_k(active_id: int, k: int) -> list[int]:
    lo = d.bin_array_index(active_id - k)
    hi = d.bin_array_index(active_id + k)
    return list(range(lo, hi + 1))


def main() -> int:
    live.load_dotenv()
    dec_path = Path(sys.argv[1])
    univ_json = Path(sys.argv[2])
    outdir = Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    rows = [json.loads(l) for l in dec_path.read_text(encoding="utf-8").splitlines() if l.strip()]
    pools = {p["idx"]: p for p in json.loads(univ_json.read_text(encoding="utf-8")).get("pools", [])}
    ev = outdir / "events.txt"
    with ev.open("w", encoding="utf-8") as f:
        for r in rows:
            if r.get("n_proto") != 1:
                continue
            f.write(f"{int(r['pool_idx'])} {int(r['n_ain'])} {int(r['n_dir'])}\n")
    by_pool = defaultdict(int)
    for r in rows:
        if r.get("n_proto") == 1:
            by_pool[int(r["pool_idx"])] += 1
    print(f"events={sum(by_pool.values())} dlmm_pools={len(by_pool)}", flush=True)
    for idx, n in sorted(by_pool.items()):
        meta = pools.get(idx)
        if not meta or meta.get("kind") != "dlmm":
            continue
        pk = meta["pubkey"]
        accs = d.get_multiple([pk])
        if not accs or not accs[0]:
            print(f"  skip {idx} no pair", flush=True)
            continue
        lb = d.parse_lbpair(accs[0]["data"])
        keys = [d.bin_array_pda(pk, i) for i in array_indexes_k(lb["active_id"], K)]
        arrs = d.get_multiple(keys)
        bins = []
        for acc in arrs:
            if acc:
                bins.extend(d.parse_bin_array(acc["data"]))
        want = {b["id"] for b in bins if abs(b["id"] - lb["active_id"]) <= K}
        use = [b for b in bins if b["id"] in want]
        path = outdir / f"pool_{idx}.bins"
        with path.open("w", encoding="utf-8") as f:
            f.write(f"{lb['active_id']} {lb['bin_step']} {len(use)}\n")
            for b in use:
                f.write(f"{b['id']} {b['x']} {b['y']}\n")
        print(f"  pool {idx} active={lb['active_id']} bins={len(use)} n={n}", flush=True)
    print(f"wrote {outdir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
