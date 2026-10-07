"""Weekly ML run: one job on one machine.

    python3 worker.py JOB.json

Job kinds (all publish with the fleet convention: files under an attempt prefix,
an immutable publication.json, then a conditional COMPLETE; a COMPLETE that
already exists means the job is done):

- index    one labels partition -> `candidate-packs index-library` + `ml-prep masters`.
           Publishes the index, the small label tables ml-prep needs, and masters.parquet.
           Also prefetches the partition's raw transactions for its later `prep` job.
- library  every published index -> train-only `build-library`, val/test `sweep-library`,
           `ml-prep library-support`. Publishes the frozen library.
- prep     one partition -> `ml-prep build` (framed-tx-v2) against the frozen library.

Progress is written to WORK/<job id>.status as JSON (state running|done|failed).
Requires TIGRIS_* in the environment or in /opt/ml/credentials.json.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import traceback
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # fleet/ is shipped next to this file
from fleet.common import encoded, identity  # noqa: E402
from fleet.store import Store  # noqa: E402

# Label tables index-library reads (candidate-packs load.rs / library.rs) plus what ml-prep reads.
INDEX_INPUTS = {
    "trigger_occurrences.parquet", "labels.parquet", "topk.parquet", "route_catalog.parquet",
    "execution_catalog.parquet", "manifest.json", "route_occurrences.parquet",
    "route-occurrence-routes.parquet", "route-occurrence-evidence.parquet", "route-occurrence-format.json",
    "markets.parquet", "routes.parquet", "derived-manifest.json", "engine-mode.json",
}
# Small label tables published next to each index so prep never re-reads the full labels.
ML_LABELS = ("trigger_occurrences.parquet", "markets.parquet", "execution_catalog.parquet", "labels.parquet",
             "manifest.json")


class Status:
    def __init__(self, path):
        self.path, self.state = Path(path), dict(state="running", started=time.time())

    def update(self, **kv):
        self.state.update(kv, t=time.time())
        tmp = self.path.with_name(self.path.name + ".pending")
        tmp.write_text(json.dumps(self.state))
        tmp.replace(self.path)


def run(cmd, log, env=None):
    with open(log, "ab") as out:
        out.write(("$ " + " ".join(map(str, cmd)) + "\n").encode())
        out.flush()
        code = subprocess.run([str(c) for c in cmd], stdout=out, stderr=subprocess.STDOUT, env=env).returncode
    if code:
        raise RuntimeError(f"{cmd[0]} {cmd[1] if len(cmd) > 1 else ''} exited {code}; see {log}")


def fetch(store, records, root, strip=""):
    """Verified download of manifest records into root (path prefix `strip` removed)."""
    root = Path(root)

    def one(rec):
        rel = rec["path"][len(strip):] if strip and rec["path"].startswith(strip) else rec["path"]
        dest = root / rel
        if dest.exists() and dest.stat().st_size == rec["size"]:
            return rec["size"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        partial = dest.with_name(dest.name + ".partial")
        store.verify(rec, partial)
        partial.replace(dest)
        return rec["size"]

    with ThreadPoolExecutor(max_workers=6) as pool:
        return sum(pool.map(one, records))


def published(store, prefix):
    marker, manifest = store.checked_manifest(prefix + "/COMPLETE")
    return manifest if marker else None


def files_under(root, sub):
    base = Path(root) / sub
    return sorted(str(p.relative_to(root)).replace(os.sep, "/") for p in base.rglob("*")
                  if p.is_file() and not p.name.endswith((".partial", ".pending", ".tmp")))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def publish(store, job, prefix, root, files, provenance):
    if job.get("dry_run"):
        # Local test: record what would be published, keep the files.
        (Path(root) / "DRY_RUN_PUBLICATION.json").write_text(json.dumps(
            dict(prefix=prefix, files=files, provenance=provenance), indent=1, default=str))
        return None
    attempt = prefix + "/attempts/" + uuid.uuid4().hex
    store.publish(prefix, identity(job), attempt, root, files, provenance)
    return attempt


def binaries_provenance(job):
    return {name: sha256_file(path) for name, path in job["bins"].items()}


# ---------------------------------------------------------------- jobs

def job_index(store, job, work, status):
    p = job["partition"]
    prefix = f"{job['run_prefix']}/index/{p['id']}"
    if published(store, prefix):
        status.update(stage="already published")
        return
    labels_manifest = published(store, f"labels/{job['labels_run']}/partitions/{p['id']}")
    if labels_manifest is None:
        raise RuntimeError("labels partition not published")
    want = [r for r in labels_manifest["files"]
            if r["path"].startswith("labels/") and r["path"].split("/")[-1] in INDEX_INPUTS]
    status.update(stage="download labels", bytes=sum(r["size"] for r in want))
    fetch(store, want, work / "src", strip="")
    labels = work / "src" / "labels"
    out = work / "out"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    status.update(stage="index-library")
    run([job["bins"]["candidate_packs"], "index-library", "--economics", labels, "--output", out / "index"], work / "job.log")
    status.update(stage="masters")
    run([job["bins"]["ml_prep"], "masters", "--labels", labels, "--out", out / "masters.parquet"], work / "job.log")
    (out / "labels").mkdir()
    for name in ML_LABELS:
        shutil.copy2(labels / name, out / "labels" / name)
    files = files_under(out, "index") + files_under(out, "labels") + ["masters.parquet"]
    status.update(stage="publish", files=len(files))
    provenance = dict(partition=p, labels_publication=labels_manifest["attempt_prefix"],
                      labels_job_sha256=labels_manifest["job_sha256"], binaries=binaries_provenance(job))
    publish(store, job, prefix, out, files, provenance)
    if not job.get("dry_run"):
        shutil.rmtree(work / "src", ignore_errors=True)
        shutil.rmtree(out, ignore_errors=True)
    if job.get("prefetch_raw"):
        status.update(stage="prefetch raw")
        prefetch_raw(store, job, status)


def raw_records(store, source_prefix):
    manifest = published(store, source_prefix)
    if manifest is None:
        raise RuntimeError("source dataset not published: " + source_prefix)
    return [r for r in manifest["files"]
            if r["path"] == "dataset/compaction.json" or r["path"].endswith(".transactions.parquet")]


def prefetch_raw(store, job, status):
    p = job["partition"]
    records = raw_records(store, p["source_prefix"])
    status.update(stage="download raw", bytes=sum(r["size"] for r in records))
    fetch(store, records, Path(job["raw_root"]) / p["id"])


def job_library(store, job, work, status):
    prefix = f"{job['run_prefix']}/library"
    if published(store, prefix):
        status.update(stage="already published")
        return
    parts = job["partitions"]
    indexes = []
    status.update(stage="download indexes", partitions=len(parts))
    for p in parts:
        manifest = published(store, f"{job['run_prefix']}/index/{p['id']}")
        if manifest is None:
            raise RuntimeError("index not published: " + p["id"])
        recs = [r for r in manifest["files"] if r["path"].startswith("index/")]
        fetch(store, recs, work / "indexes" / p["id"], strip="index/")
        indexes.append(work / "indexes" / p["id"])
    w = job["windows"]
    out = work / "out"
    config = json.loads(Path(job["library_config"]).read_text())
    config.update(train_master_tx_id_min=w["train"]["global_min"], train_master_tx_id_max=w["train"]["global_max"])
    config_text = json.dumps(config, indent=2, sort_keys=True) + "\n"
    windows_text = json.dumps(w, indent=2, sort_keys=True) + "\n"
    # A retry keeps a finished build (library.json is written last) from the same config and windows:
    # the build is the expensive step, and later steps can fail on their own.
    same = all((out / name).exists() and (out / name).read_text() == text
               for name, text in (("library-train.json", config_text), ("windows.json", windows_text)))
    if not same:
        shutil.rmtree(out, ignore_errors=True)
    elif not (out / "library" / "library.json").exists():
        shutil.rmtree(out / "library", ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    (out / "library-train.json").write_text(config_text)
    (out / "windows.json").write_text(windows_text)
    log = work / "job.log"
    cp = job["bins"]["candidate_packs"]
    if not (out / "library" / "library.json").exists():
        status.update(stage="build-library")
        run([cp, "build-library", "--economics", *indexes, "--output", out / "library", "--config", out / "library-train.json"], log)
    for split in ("val", "test"):
        target = out / f"sweep-library-{split}.json"
        if target.exists():
            continue
        status.update(stage="sweep " + split)
        partial = target.with_name(target.name + ".partial")
        partial.unlink(missing_ok=True)
        run([cp, "sweep-library", "--library", out / "library", "--economics", *indexes,
             "--master-min", w[split]["global_min"], "--master-max", w[split]["global_max"],
             "--ks", "1,2,3,5,8,16,32,64", "--output", partial], log)
        partial.rename(target)
    status.update(stage="library-support")
    run([job["bins"]["ml_prep"], "library-support", "--library", out / "library", "--index", *indexes,
         "--train-min", w["train"]["global_min"], "--train-max", w["train"]["global_max"], "--out", out / "library"], log)
    files = files_under(out, "library") + ["library-train.json", "windows.json", "sweep-library-val.json",
                                           "sweep-library-test.json"]
    status.update(stage="publish", files=len(files))
    publish(store, job, prefix, out, files, dict(partitions=[p["id"] for p in parts], windows=w,
                                                   binaries=binaries_provenance(job)))
    shutil.rmtree(work, ignore_errors=True)


def job_prep(store, job, work, status):
    p = job["partition"]
    prefix = f"{job['run_prefix']}/prep/{p['id']}"
    if published(store, prefix):
        status.update(stage="already published")
        return
    index = published(store, f"{job['run_prefix']}/index/{p['id']}")
    library = published(store, f"{job['run_prefix']}/library")
    if index is None or library is None:
        raise RuntimeError("index or library not published")
    status.update(stage="download index+library")
    fetch(store, [r for r in index["files"] if r["path"].startswith(("index/", "labels/"))], work / "in")
    fetch(store, library["files"], work / "lib")
    prefetch_raw(store, job, status)
    raw = Path(job["raw_root"]) / p["id"] / "dataset"
    out = work / "prep"
    shutil.rmtree(out, ignore_errors=True)
    lib = work / "lib"
    status.update(stage="ml-prep build")
    cmd = [job["bins"]["ml_prep"], "build", "--labels", work / "in" / "labels", "--positives", work / "in" / "index",
           "--packs", lib / "library", "--raw-dataset", raw, "--out", out,
           "--slots-per-part", "250", "--threads", str(job["threads"]), "--max-depth", "64",
           "--unknown-pair-ppm", str(job["unknown_pair_ppm"]),
           "--min-slot", str(p["owned_first_slot"]), "--max-slot", str(p["owned_end_slot"]),
           "--partition-index", str(p["index"]), "--train-routes", lib / "library" / "train_routes.parquet",
           "--dataset-id", f"{job['labels_run']}/{p['id']}"]
    for name in ("windows.json", "library-train.json", "sweep-library-val.json", "sweep-library-test.json"):
        cmd += ["--provenance", lib / name]
    cmd += ["--provenance", lib / "library" / "library.json"]
    run(cmd, work / "job.log")
    files = sorted(str(f.relative_to(out)).replace(os.sep, "/") for f in out.rglob("*") if f.is_file())
    status.update(stage="publish", files=len(files))
    publish(store, job, prefix, out, files, dict(partition=p, index_publication=index["attempt_prefix"],
                                                   library_publication=library["attempt_prefix"],
                                                   binaries=binaries_provenance(job)))
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(Path(job["raw_root"]) / p["id"], ignore_errors=True)


JOBS = dict(index=job_index, library=job_library, prep=job_prep)


def load_credentials():
    path = Path(os.environ.get("ML_CREDENTIALS", "/opt/ml/credentials.json"))
    if path.exists():
        for k, v in json.loads(path.read_text()).items():
            os.environ.setdefault(k, v)


def main():
    job = json.loads(Path(sys.argv[1]).read_text())
    load_credentials()
    work = Path(job["work_root"]) / job["id"]
    work.mkdir(parents=True, exist_ok=True)
    status = Status(Path(job["work_root"]) / (job["id"] + ".status"))
    status.update(kind=job["kind"], job_sha256=identity(job))
    try:
        store = Store(workers=8)
        JOBS[job["kind"]](store, job, work, status)
        status.update(state="done", stage="done")
    except Exception as error:  # surface everything to the controller
        status.update(state="failed", error=f"{type(error).__name__}: {error}", trace=traceback.format_exc()[-4000:])
        raise


if __name__ == "__main__":
    main()
