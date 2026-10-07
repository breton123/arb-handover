"""Download a published weekly run (library + every prepared partition) for analysis.

    python weekly/fetch.py --out /data/bsc/firm-ml/week/run-001 [--partitions ID ...]
    python weekly/fetch.py --out ./data --bundle pool-ev-v1        # the paper-trading model bundle only

Writes `<out>/library/...` and `<out>/prep/<partition>/...` (the layout `firm_ml.week.open_week` reads).
Only COMPLETE publications are read and every download is verified against its manifest sha256. Files
already present at the recorded size are skipped, so an interrupted fetch resumes.

Credentials: TIGRIS_* in the environment, or `--credentials` JSON (the same keys).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
LABELS_RUN = "labeller-week-20260925-001"
RUN_ID = "run-001"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--run-prefix", default=f"ml/{LABELS_RUN}/{RUN_ID}")
    ap.add_argument("--partitions", nargs="*", help="only these partition ids (default: every partition in PLAN.json)")
    ap.add_argument("--bundle", help="fetch only this model bundle (bundles/<name>) into <out>/bundles/<name>")
    ap.add_argument("--credentials", type=Path)
    ap.add_argument("--workers", type=int, default=16)
    args = ap.parse_args()
    if args.credentials:
        os.environ.update(json.loads(args.credentials.read_text()))
    sys.path.insert(0, str(HERE))
    if not (HERE / "fleet").exists():  # local checkout: fleet/ lives in the labeller repo
        sys.path.insert(0, str(HERE.parents[2] / "firm-dataset-labeller"))
    from worker import Store, fetch, published  # same verified download path as the fleet

    store = Store(workers=args.workers)
    if args.bundle:
        name = f"bundles/{args.bundle}"
        manifest = published(store, f"{args.run_prefix}/{name}")
        if manifest is None:
            sys.exit(f"{name} is not published under {args.run_prefix}")
        fetch(store, manifest["files"], args.out / name)
        print(f"{name}: {len(manifest['files'])} files -> {args.out / name}")
        return
    ids = args.partitions
    if not ids:
        plan = store.get(f"{args.run_prefix}/PLAN.json")
        if plan is None:
            sys.exit(f"no PLAN.json under {args.run_prefix}")
        ids = [p["id"] for p in json.loads(plan)["partitions"]]
    jobs = [("library", args.out / "library")] + [(f"prep/{i}", args.out / "prep" / i) for i in ids]
    for name, _ in jobs:
        if published(store, f"{args.run_prefix}/{name}") is None:
            sys.exit(f"{name} is not published under {args.run_prefix}")

    def one(job):
        name, dest = job
        manifest = published(store, f"{args.run_prefix}/{name}")
        fetch(store, manifest["files"], dest)
        return name, len(manifest["files"]), sum(r.get("size", 0) for r in manifest["files"])

    total = 0
    with ThreadPoolExecutor(4) as pool:
        for name, n, size in pool.map(one, jobs):
            total += size
            print(f"{name}: {n} files, {size / 1e9:.2f} GB", flush=True)
    print(f"done: {len(jobs)} publications, {total / 1e9:.1f} GB -> {args.out}")


if __name__ == "__main__":
    main()
