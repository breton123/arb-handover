"""Publish a bundle directory to Tigris with the run's publication convention (attempt + publication.json + COMPLETE)."""
import json, os, sys
from pathlib import Path
R = Path(os.environ.get("FIRM_ML_ROOT", "/data/bsc/captures/firm-ml"))
os.environ.update(json.load(open(R / "tigris.json")))
sys.path.insert(0, str(R / "code" / "weekly"))
from worker import Store, publish, published, sha256_file

root = R / "bundles" / "pool-ev-v1"
prefix = "ml/labeller-week-20260925-001/run-001/bundles/pool-ev-v1"
store = Store(workers=8)
if published(store, prefix):
    print("already published"); sys.exit(0)
files = sorted(p.name for p in root.iterdir() if p.is_file())
job = {"id": "bundle-pool-ev-v1", "kind": "bundle", "manifest_sha256": sha256_file(root / "manifest.json")}
attempt = publish(store, job, prefix, root, files, {"bundle": "pool-ev-v1", "manifest_sha256": job["manifest_sha256"]})
m = published(store, prefix)
print("published", prefix, attempt, len(m["files"]), "files", sum(r["size"] for r in m["files"]) / 1e6, "MB")
