"""Weekly ML run controller: Cherry servers + Tigris, three phases.

    uv run --with boto3 python ml/weekly/orchestrate.py plan
    uv run --with boto3 python ml/weekly/orchestrate.py launch --count 41
    uv run --with boto3 python ml/weekly/orchestrate.py run
    uv run --with boto3 python ml/weekly/orchestrate.py status
    uv run --with boto3 python ml/weekly/orchestrate.py terminate

Phases (see worker.py): index per partition (+ raw prefetch) -> global slot windows ->
library (train-only, frozen) -> prep per partition. Every output is published under
RUN_PREFIX with publication.json + COMPLETE; a rerun skips anything already COMPLETE.

Reuses firm-dataset-labeller/fleet: Cherry API, Store, provisioning bookkeeping
(create intent persisted before POST, so a lost response never double-creates),
bootstrap/host-key handling, and ownership-checked deletes. Credentials come from the
same env files as the labeller fleet and are never written to the deployment directory.
"""
import argparse
import base64
import io
import json
import os
import subprocess
import sys
import tarfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

OFFLINE = Path(__file__).resolve().parents[2]
LABELLER = OFFLINE.parent / "firm-dataset-labeller"
sys.path.insert(0, str(LABELLER))
from fleet.cherry import Cherry, discover, provision, reconcile  # noqa: E402
from fleet.common import identity, load_env, require, save, utc  # noqa: E402
from fleet.deploy import bootstrap_ready, remote  # noqa: E402
from fleet.store import Store  # noqa: E402

LABELS_RUN = "labeller-week-20260925-001"
RUN_ID = "run-001"
RUN_PREFIX = f"ml/{LABELS_RUN}/{RUN_ID}"
DEPLOY = Path(__file__).resolve().parent / "deployments" / f"{LABELS_RUN}-{RUN_ID}"
PLAN_SLUG, REGION, IMAGE = "e5-1650v4", "LT-Siauliai", "ubuntu_24_04_64bit"
RUST_TOOLCHAIN = "1.95.0"
THREADS = 12
UNKNOWN_PAIR_PPM = 125_000
FRACTIONS = (0.70, 0.15, 0.15)
CREDENTIALS = ("TIGRIS_URL_S3", "TIGRIS_REGION", "TIGRIS_ACCESS_KEY_ID", "TIGRIS_SECRET_ACCESS_KEY", "TIGRIS_BUCKET")
LOCK = threading.Lock()
SRC = "/opt/ml/src"
BINS = dict(candidate_packs=f"{SRC}/labeller/target/release/candidate-packs",
            ml_prep=f"{SRC}/offline/target/release/ml-prep")


def log(*parts):
    line = f"[{time.strftime('%H:%M:%S')}] " + " ".join(str(p) for p in parts)
    print(line, flush=True)
    with LOCK:
        with (DEPLOY / "run.log").open("a", encoding="utf-8") as f:
            f.write(line + "\n")


def env():
    load_env(LABELLER.parent / "firm-firedancer-collector" / ".env", prefix="TIGRIS_")
    load_env(LABELLER.parent / "the-firm-dataset" / ".env", prefix="CHERRY_")
    require(all(os.environ.get(k) for k in CREDENTIALS), "missing Tigris credentials")


def fleet_ids():
    fleet = json.loads((LABELLER / "deployments" / LABELS_RUN / "fleet.json").read_text())
    return str(fleet["team_id"]), str(fleet["project_id"])


def load_state():
    path = DEPLOY / "state.json"
    return json.loads(path.read_text()) if path.exists() else dict(machines=[], jobs={})


def persist(state):
    # Windows refuses the atomic rename while any reader holds state.json open.
    with LOCK:
        for attempt in range(20):
            try:
                return save(DEPLOY / "state.json", state)
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.25)


# ---------------------------------------------------------------- plan

def plan_command(args):
    env()
    DEPLOY.mkdir(parents=True, exist_ok=True)
    store = Store(workers=8)
    labels_plan = json.loads(store.get(f"labels/{LABELS_RUN}/PLAN.json"))
    parts = []
    for p in sorted(labels_plan["partitions"], key=lambda p: p["first_slot"]):
        marker, manifest = store.checked_manifest(f"labels/{LABELS_RUN}/partitions/{p['id']}/COMPLETE")
        if marker is None:
            continue
        prov = manifest["provenance"]
        need = sum(f["size"] for f in manifest["files"] if f["path"].startswith("labels/"))
        parts.append(dict(id=p["id"], index=len(parts), source_prefix=p["source_prefix"],
                          owned_first_slot=prov["owned_first_slot"], owned_end_slot=prov["owned_end_slot"],
                          label_bytes=need))
    plan = dict(schema=1, labels_run=LABELS_RUN, run_prefix=RUN_PREFIX, partitions=parts, fractions=FRACTIONS,
                unknown_pair_ppm=UNKNOWN_PAIR_PPM, plan_slug=PLAN_SLUG, region=REGION, image=IMAGE,
                rust_toolchain=RUST_TOOLCHAIN, global_id_shift=40)
    save(DEPLOY / "plan.json", plan)
    log(f"plan: {len(parts)} published partitions of {len(labels_plan['partitions'])}; "
        f"label bytes {sum(p['label_bytes'] for p in parts) / 1e12:.2f} TB")


# ---------------------------------------------------------------- bundle and bootstrap

def bundle():
    """Source tarball: this workspace (ml-prep, tx-features), the labeller crates, the worker."""
    buf = io.BytesIO()
    skip = ("target", "tests", "__pycache__", ".pytest_cache")

    def add(tar, path, arc):
        if path.is_dir():
            for child in sorted(path.iterdir()):
                if child.name in skip:
                    continue
                add(tar, child, f"{arc}/{child.name}")
        else:
            info = tar.gettarinfo(str(path), arc)
            info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
            with path.open("rb") as f:
                tar.addfile(info, f)

    with tarfile.open(fileobj=buf, mode="w:gz", compresslevel=6) as tar:
        for rel in ("Cargo.toml", "Cargo.lock", "crates/tx-features", "crates/tx-features-cli", "ml/prep"):
            add(tar, OFFLINE / rel, f"offline/{rel}")
        for rel in ("Cargo.toml", "Cargo.lock", "crates/economics", "crates/candidate-packs", "crates/dataset-reader",
                    "config/library-v1.json"):
            add(tar, LABELLER / rel, f"labeller/{rel}")
        add(tar, Path(__file__).resolve().parent / "worker.py", "weekly/worker.py")
        for rel in ("__init__.py", "common.py", "store.py"):
            add(tar, LABELLER / "fleet" / rel, f"weekly/fleet/{rel}")
    return buf.getvalue()


def cloud_init(host_id):
    commands = [
        "mkdir -p /opt/ml/jobs /opt/ml/work /opt/ml/raw && chmod 700 /opt/ml",
        "python3 -m venv /opt/ml/venv",
        "/opt/ml/venv/bin/pip install -q boto3==1.43.40 botocore==1.43.40",
        "export HOME=/root",
        "curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | "
        f"sh -s -- -y --profile minimal --default-toolchain {RUST_TOOLCHAIN}",
        f"echo {host_id} > /opt/ml/HOST_ID",
        "touch /opt/ml/BOOTSTRAP_READY",
    ]
    data = "#cloud-config\n" + json.dumps(dict(
        package_update=True, packages=["python3", "python3-venv", "build-essential", "pkg-config", "zstd",
                                       "ca-certificates", "curl"],
        hostname=host_id, preserve_hostname=False, manage_etc_hosts=True,
        runcmd=[["bash", "-euc", "\n".join(commands)]])) + "\n"
    require(len(base64.b64encode(data.encode())) <= 65535, "cloud-init too large")
    return data


def ready_check(host_id):
    return f'test -f /opt/ml/BOOTSTRAP_READY && test "$(cat /opt/ml/HOST_ID)" = {host_id}'


def ssh(machine, script, data=None, timeout=120):
    return remote(machine, DEPLOY, Path(os.environ["CHERRY_SSH_KEY"]), ready_check(machine["host_id"]) + " && " + script,
                  data, timeout=timeout)


BUILD = (f"cd {SRC}/offline && /root/.cargo/bin/cargo build --release --locked -p ml-prep > /opt/ml/build.log 2>&1 && "
         f"cd {SRC}/labeller && /root/.cargo/bin/cargo build --release --locked -p firm-candidate-packs >> /opt/ml/build.log 2>&1 && "
         "touch /opt/ml/BUILD_OK || touch /opt/ml/BUILD_FAILED")


def launch_command(args):
    env()
    plan = json.loads((DEPLOY / "plan.json").read_text())
    team, project = fleet_ids()
    api = Cherry(os.environ["CHERRY_KEY"], timeout=300)
    key = Path(os.environ["CHERRY_SSH_KEY"])
    ids = [int(i) for i in os.environ["CHERRY_SSH_KEY_IDS"].replace(",", " ").split()]
    public = subprocess.check_output(["ssh-keygen", "-y", "-P", "", "-f", str(key)], text=True).split()[:2]
    require(any(api.call("GET", "/ssh-keys/" + str(i))["key"].split()[:2] == public for i in ids),
            "SSH key does not match registered key IDs")
    state = load_state()
    existing = len(state["machines"])
    # --count is per role: workers (no role) or e.g. one large-memory "library" machine.
    want = args.count - len([m for m in state["machines"] if not m.get("terminated") and m.get("role") == args.role])
    if want > 0:
        capacity = discover(api, team)
        rows = sorted((r for r in capacity["ranked"] if r["plan"] == args.plan and r["region"] == args.region
                       and r["stock_qty"] > 0 and not r["disabled"] and r.get("hourly")),
                      key=lambda r: r["hourly"]["price"])
        require(rows, f"no {args.plan} stock in {args.region}")
        best = rows[0]
        want = min(want, best["stock_qty"])
        for i in range(existing, existing + want):
            m = dict(host_id=f"mlw-{RUN_ID}-{i:02}", plan=args.plan, region=args.region,
                     prebuilt_id=best.get("prebuilt_id"), hourly=best["hourly"])
            if args.role:
                m["role"] = args.role
            state["machines"].append(m)
        persist(state)
        log(f"launch: {want} x {args.plan} in {args.region} at {best['hourly']['price']} {best['hourly']['currency']}/h "
            f"(stock {best['stock_qty']}){' role ' + args.role if args.role else ''}")
    payload_bundle = bundle()
    bundle_sha = identity(dict(b=base64.b64encode(payload_bundle).decode()))
    credentials = json.dumps({k: os.environ[k] for k in CREDENTIALS}).encode()
    cherry_run = f"ml-{LABELS_RUN}-{RUN_ID}"

    def start(m):
        try:
            data = cloud_init(m["host_id"])
            payload = dict(plan=m["plan"], region=m["region"], hostname=m["host_id"], image=IMAGE, ssh_keys=ids,
                           cycle="hourly", spot_market=False,
                           tags=dict(dataset_run=cherry_run, dataset_worker=m["host_id"]),
                           user_data=base64.b64encode(data.encode()).decode())
            if m.get("prebuilt_id") is not None:
                payload["prebuilt_id"] = m["prebuilt_id"]
            provision(api, project, cherry_run, m, payload, lambda: persist(state))
            deadline = time.time() + 3600
            while True:
                reconcile(api, project, cherry_run, m)
                persist(state)
                if m["server"].get("state") == "active" and m["server"]["ips"] and \
                        bootstrap_ready(m, DEPLOY, key, ready_check(m["host_id"])):
                    break
                require(time.time() < deadline, m["host_id"] + ": bootstrap timeout")
                time.sleep(30)
            if not m.get("built"):
                code, _, err = ssh(m, f"rm -rf {SRC} && mkdir -p {SRC} && tar xzf - -C {SRC}", payload_bundle, timeout=600)
                require(code == 0, m["host_id"] + ": bundle delivery failed " + err[-300:])
                code, _, err = ssh(m, "umask 077 && cat > /opt/ml/credentials.json", credentials)
                require(code == 0, m["host_id"] + ": credentials delivery failed")
                ssh(m, f"rm -f /opt/ml/BUILD_OK /opt/ml/BUILD_FAILED; nohup setsid bash -c '{BUILD}' > /dev/null 2>&1 &")
                while True:
                    code, out, _ = ssh(m, "ls /opt/ml/BUILD_OK /opt/ml/BUILD_FAILED 2>/dev/null; df -BG --output=avail / | tail -1")
                    if "BUILD_FAILED" in out:
                        _, tail, _ = ssh(m, "tail -20 /opt/ml/build.log")
                        raise RuntimeError(m["host_id"] + ": build failed\n" + tail)
                    if "BUILD_OK" in out:
                        m["disk_free"] = out.strip().splitlines()[-1].strip()
                        break
                    time.sleep(20)
                m.update(built=utc(), bundle_sha=bundle_sha)
                persist(state)
            log(m["host_id"], "ready", m["server"]["ips"], "disk", m.get("disk_free"))
        except Exception as error:
            m["launch_error"] = f"{type(error).__name__}: {error}"[-500:]
            persist(state)
            log(m["host_id"], "LAUNCH FAILED", m["launch_error"])

    live = [m for m in state["machines"] if not m.get("terminated") and m.get("role") == args.role]
    with ThreadPoolExecutor(max_workers=max(1, len(live))) as pool:
        list(pool.map(start, live))
    log(f"launch: {sum(1 for m in live if m.get('built'))}/{len(live)} machines ready")


# ---------------------------------------------------------------- run

def published(store, prefix):
    marker, _ = store.checked_manifest(prefix + "/COMPLETE")
    return marker is not None


def job_spec(kind, plan, partition=None, **extra):
    job = dict(kind=kind, run_prefix=RUN_PREFIX, labels_run=LABELS_RUN, work_root="/opt/ml/work", raw_root="/opt/ml/raw",
               bins=BINS, threads=THREADS, unknown_pair_ppm=plan["unknown_pair_ppm"], **extra)
    if partition is not None:
        job["partition"] = {k: partition[k] for k in ("id", "index", "source_prefix", "owned_first_slot", "owned_end_slot")}
        job["id"] = f"{kind}-{partition['id']}"
    else:
        job["id"] = kind
    return job


def execute(state, m, job, store, prefix):
    """Run one job on machine m; True when its COMPLETE is visible in Tigris."""
    job_json = json.dumps(job).encode()
    code, _, err = ssh(m, f"cat > /opt/ml/jobs/{job['id']}.json", job_json)
    if code:
        raise RuntimeError("job upload failed: " + err[-300:])
    ssh(m, f"rm -f /opt/ml/work/{job['id']}.status; nohup setsid /opt/ml/venv/bin/python {SRC}/weekly/worker.py "
           f"/opt/ml/jobs/{job['id']}.json > /opt/ml/jobs/{job['id']}.log 2>&1 &")
    state["jobs"][job["id"]] = dict(machine=m["host_id"], started=utc(), state="running")
    persist(state)
    unreachable = 0
    while True:
        time.sleep(30)
        try:
            code, out, _ = ssh(m, f"cat /opt/ml/work/{job['id']}.status 2>/dev/null || echo '{{}}'", timeout=60)
        except subprocess.TimeoutExpired:
            code, out = 1, ""
        if code:
            unreachable += 1
            if unreachable > 30:
                raise RuntimeError(m["host_id"] + " unreachable")
            continue
        unreachable = 0
        try:
            st = json.loads(out or "{}")
        except json.JSONDecodeError:
            continue
        if st.get("stage") and st.get("stage") != state["jobs"][job["id"]].get("stage"):
            state["jobs"][job["id"]].update(stage=st["stage"])
            persist(state)
        if st.get("state") == "done":
            ok = published(store, prefix)
            state["jobs"][job["id"]].update(state="done" if ok else "missing", finished=utc())
            persist(state)
            return ok
        if st.get("state") == "failed":
            state["jobs"][job["id"]].update(state="failed", error=st.get("error"))
            persist(state)
            _, tail, _ = ssh(m, f"tail -15 /opt/ml/work/{job['id']}/job.log 2>/dev/null")
            raise RuntimeError(f"{job['id']} failed on {m['host_id']}: {st.get('error')}\n{tail}")


def run_phase(state, store, machines, jobs, affinity=None, retries=2):
    """jobs: list of (job, prefix). Each machine pulls jobs; failures are retried elsewhere."""
    pending = [j for j in jobs if not published(store, j[1])]
    log(f"phase: {len(jobs) - len(pending)} already published, {len(pending)} to run on {len(machines)} machines")
    queue = list(pending)
    attempts = {}
    qlock = threading.Lock()

    def take(m):
        with qlock:
            if not queue:
                return None
            if affinity:
                # Own partition first (its raw data is already on disk), then orphans, then steal.
                for i, j in enumerate(queue):
                    if affinity.get(j[0]["id"]) == m["host_id"]:
                        return queue.pop(i)
                live = {x["host_id"] for x in machines}
                for i, j in enumerate(queue):
                    if affinity.get(j[0]["id"]) not in live:
                        return queue.pop(i)
            return queue.pop(0)

    def worker(m):
        while True:
            item = take(m)
            if item is None:
                return
            job, prefix = item
            try:
                ok = execute(state, m, job, store, prefix)
                log(job["id"], "done" if ok else "finished without COMPLETE", "on", m["host_id"])
                if not ok:
                    raise RuntimeError("COMPLETE missing")
            except Exception as error:
                n = attempts.get(job["id"], 0) + 1
                attempts[job["id"]] = n
                log(job["id"], f"attempt {n} failed:", str(error)[-600:])
                if n <= retries:
                    with qlock:
                        queue.append(item)
                        if affinity:
                            affinity.pop(job["id"], None)

    with ThreadPoolExecutor(max_workers=len(machines)) as pool:
        list(pool.map(worker, machines))
    missing = [j[0]["id"] for j in jobs if not published(store, j[1])]
    require(not missing, f"phase incomplete: {missing}")


def compute_windows(plan, store):
    """Global 70/15/15 slot windows over the owned slots of all partitions, mapped to global ids."""
    import polars as pl
    frames = []
    for p in plan["partitions"]:
        _, manifest = store.checked_manifest(f"{RUN_PREFIX}/index/{p['id']}/COMPLETE")
        rec = next(r for r in manifest["files"] if r["path"] == "masters.parquet")
        dest = DEPLOY / "masters" / f"{p['id']}.parquet"
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            store.verify(rec, dest)
        m = pl.read_parquet(dest).filter(pl.col("slot").is_between(p["owned_first_slot"], p["owned_end_slot"]))
        require((m["slot"].diff().drop_nulls() >= 0).all(), p["id"] + ": masters not monotonic in slot")
        require(m.height == 0 or m["master_tx_id"].max() < 1 << 40, p["id"] + ": master_tx_id exceeds the global id shift")
        frames.append(m.with_columns(pl.lit(p["index"], pl.UInt64).alias("partition"),
                                     (pl.lit(p["index"] << 40, pl.UInt64) + pl.col("master_tx_id").cast(pl.UInt64)).alias("global_id")))
    allm = pl.concat(frames)
    slots = allm["slot"].unique().sort()
    n = slots.len()
    c1, c2 = int(round(n * FRACTIONS[0])), int(round(n * (FRACTIONS[0] + FRACTIONS[1])))
    bounds = dict(train=(slots[0], slots[c1 - 1]), val=(slots[c1], slots[c2 - 1]), test=(slots[c2], slots[n - 1]))
    windows = dict(fractions=list(FRACTIONS), distinct_slots=n, global_id_shift=40,
                   partitions=[{k: p[k] for k in ("id", "index", "owned_first_slot", "owned_end_slot")} for p in plan["partitions"]])
    for name, (lo, hi) in bounds.items():
        w = allm.filter(pl.col("slot").is_between(lo, hi))
        windows[name] = dict(slot_min=int(lo), slot_max=int(hi), global_min=int(w["global_id"].min()),
                             global_max=int(w["global_id"].max()), master_txs=w.height)
    require(windows["train"]["global_max"] < windows["val"]["global_min"] <= windows["val"]["global_max"]
            < windows["test"]["global_min"], "windows overlap in global id space")
    save(DEPLOY / "windows.json", windows)
    return windows


def run_command(args):
    env()
    plan = json.loads((DEPLOY / "plan.json").read_text())
    state = load_state()
    store = Store(workers=8)
    store.immutable_json(f"{RUN_PREFIX}/PLAN.json", plan)
    machines = [m for m in state["machines"] if m.get("built") and not m.get("terminated")]
    require(machines, "no ready machines; run launch first")
    # Phase 1: index (largest labels first) with raw prefetch for the later prep job.
    order = sorted(plan["partitions"], key=lambda p: -p["label_bytes"])
    index_jobs = [(job_spec("index", plan, p, prefetch_raw=True), f"{RUN_PREFIX}/index/{p['id']}") for p in order]
    run_phase(state, store, machines, index_jobs)
    # Phase 2: windows (local) and the library (one machine).
    windows = compute_windows(plan, store)
    log("windows:", json.dumps({k: windows[k] for k in ("train", "val", "test")}))
    lib = job_spec("library", plan, partitions=[{k: p[k] for k in ("id", "index")} for p in plan["partitions"]],
                   windows=windows, library_config=f"{SRC}/labeller/config/library-v1.json")
    # The library holds every train partition's positives in memory; prefer a large-memory machine.
    library_hosts = [m for m in machines if m.get("role") == "library"] or machines[:1]
    run_phase(state, store, library_hosts, [(lib, f"{RUN_PREFIX}/library")])
    # Phase 3: prep, preferring the machine that already holds the partition's raw data.
    affinity = {}
    for p in plan["partitions"]:
        ran = state["jobs"].get(f"index-{p['id']}", {})
        if ran.get("machine"):
            affinity[f"prep-{p['id']}"] = ran["machine"]
    prep_jobs = [(job_spec("prep", plan, p), f"{RUN_PREFIX}/prep/{p['id']}") for p in order]
    run_phase(state, store, machines, prep_jobs, affinity=affinity)
    log("run complete: every partition indexed and prepared; library frozen at", f"{RUN_PREFIX}/library")
    if args.terminate:
        terminate(state, force=False)


# ---------------------------------------------------------------- status / terminate

def status_command(args):
    state = load_state()
    jobs = state.get("jobs", {})
    by = {}
    for j in jobs.values():
        by[j["state"]] = by.get(j["state"], 0) + 1
    live = [m for m in state["machines"] if not m.get("terminated")]
    stages = {}
    for m in live:
        # launch_error can be stale: a relaunch retries every unbuilt machine (run.log has live failures).
        stage = "built" if m.get("built") else \
            "server:" + str((m.get("server") or {}).get("state") or (m.get("server") or {}).get("status") or "none")
        stages[stage] = stages.get(stage, 0) + 1
    print(json.dumps(dict(machines=len(live), ready=stages.get("built", 0), launch=stages, jobs=by), indent=1))
    for jid, j in sorted(jobs.items()):
        if j["state"] != "done":
            print(f"  {jid:40s} {j['state']:8s} {j.get('stage', '')} {j.get('machine', '')} {str(j.get('error', ''))[:80]}")


def terminate(state, force=False, hosts=None):
    env()
    _, project = fleet_ids()
    api = Cherry(os.environ["CHERRY_KEY"])
    cherry_run = f"ml-{LABELS_RUN}-{RUN_ID}"
    for m in state["machines"]:
        if m.get("terminated") or not m.get("server") or (hosts is not None and m["host_id"] not in hosts):
            continue
        if not reconcile(api, project, cherry_run, m):
            log(m["host_id"], "server absent; marking terminated")
            m["terminated"] = utc()
            persist(state)
            continue
        server = api.call("GET", "/servers/" + str(m["server"]["id"]), fields="server,project,id,hostname,tags")
        require(server["hostname"] == m["host_id"] and str(server["project"]["id"]) == project and
                server.get("tags", {}).get("dataset_run") == cherry_run, m["host_id"] + ": ownership differs")
        m["delete_intent"] = utc()
        persist(state)
        api.call("DELETE", "/servers/" + str(m["server"]["id"]))
        m["terminated"] = utc()
        persist(state)
        log(m["host_id"], "terminated")


def terminate_command(args):
    state = load_state()
    hosts = None
    if args.unbuilt:
        hosts = {m["host_id"] for m in state["machines"] if not m.get("built")}
    if args.hosts:
        hosts = (hosts or set()) | set(args.hosts)
    terminate(state, force=args.force, hosts=hosts)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan")
    p = sub.add_parser("launch")
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--plan", default=PLAN_SLUG)
    p.add_argument("--region", default=REGION)
    p.add_argument("--role", default=None, help="e.g. library: a large-memory machine for the library job")
    p = sub.add_parser("run")
    p.add_argument("--terminate", action="store_true", help="delete the servers once every prep is COMPLETE")
    sub.add_parser("status")
    p = sub.add_parser("terminate")
    p.add_argument("--force", action="store_true")
    p.add_argument("--hosts", nargs="+", help="only these host ids")
    p.add_argument("--unbuilt", action="store_true", help="only machines that never finished building")
    args = parser.parse_args()
    DEPLOY.mkdir(parents=True, exist_ok=True)
    dict(plan=plan_command, launch=launch_command, run=run_command, status=status_command,
         terminate=terminate_command)[args.cmd](args)


if __name__ == "__main__":
    main()
