"""Publish a bundle directory to Tigris with the run's publication convention (attempt + publication.json + COMPLETE).

    python experiments/publish_bundle.py --name landing-v1 [--root /data/bsc/captures/firm-ml]

Reads `<root>/bundles/<name>/` and publishes it to `ml/labeller-week-20260925-001/run-001/bundles/<name>`.
Credentials: `<root>/tigris.json` (TIGRIS_* keys). Fetch with `weekly/fetch.py --bundle <name>`.
"""
import argparse
import json
import os
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--name", required=True)
ap.add_argument("--root", type=Path, default=Path(os.environ.get("FIRM_ML_ROOT", "/data/bsc/captures/firm-ml")))
ap.add_argument("--run-prefix", default="ml/labeller-week-20260925-001/run-001")
args = ap.parse_args()

os.environ.update(json.load(open(args.root / "tigris.json")))
sys.path.insert(0, str(args.root / "code" / "weekly"))
from worker import Store, publish, published, sha256_file  # noqa: E402

root = args.root / "bundles" / args.name
prefix = f"{args.run_prefix}/bundles/{args.name}"
store = Store(workers=8)
if published(store, prefix):
    sys.exit(f"{prefix} is already published; bundles are immutable, use a new name")
files = sorted(p.name for p in root.iterdir() if p.is_file())
job = {"id": f"bundle-{args.name}", "kind": "bundle", "manifest_sha256": sha256_file(root / "manifest.json")}
attempt = publish(store, job, prefix, root, files, {"bundle": args.name, "manifest_sha256": job["manifest_sha256"]})
m = published(store, prefix)
print("published", prefix, attempt, len(m["files"]), "files", round(sum(r["size"] for r in m["files"]) / 1e6, 1), "MB")
