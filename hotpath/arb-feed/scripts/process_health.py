#!/usr/bin/env python3
"""PID-alive is not health. Heartbeats / counters / capture mtime must move.

Writes /dev/shm/PROCESS_HEALTH.json so ENOSPC on root cannot hide a dead worker.
"""
from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path

OUT = Path("/dev/shm/PROCESS_HEALTH.json")
PREV = Path("/dev/shm/PROCESS_HEALTH.prev.json")
HB = Path("/dev/shm/arb_state008_health.json")
METRICS = Path("/home/louis/captures/state008/METRICS.json")
STATS = Path("/home/louis/captures/paper_orbit/stats.json")
CAPTURE = Path(os.environ.get("CAPTURE_DIR", "/data/bsc/captures/orbitflare"))
PREFIX = os.environ.get("PREFIX", "orbitflare")


def pgrep(pat: str) -> int | None:
    r = subprocess.run(["pgrep", "-n", "-f", pat], capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        return None
    try:
        return int(r.stdout.strip().splitlines()[0])
    except ValueError:
        return None


def load(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def newest_cap() -> tuple[str | None, int, int]:
    caps = [p for p in CAPTURE.glob("orbitflare-*.cap") if p.is_file() and not p.is_symlink()]
    caps.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    if not caps:
        return None, 0, 99999
    p = caps[0]
    mt = int(p.stat().st_mtime)
    return str(p), mt, int(time.time()) - mt


def root_free_gb() -> int:
    r = subprocess.run(["df", "-P", "/"], capture_output=True, text=True, check=True)
    kb = int(r.stdout.splitlines()[1].split()[3])
    return kb // 1024 // 1024


def main() -> int:
    now = time.time()
    ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    prev = load(PREV)

    feed_pid = pgrep(f"feed_live .*--prefix {PREFIX}") or pgrep("/feed_live ")
    paper_pid = pgrep("/paper_orbit ") or pgrep("paper_orbit --dir")
    state_pid = pgrep("shyft/state008.py") or pgrep("/state008.py")

    cap_path, cap_mtime, cap_age = newest_cap()
    feed_status = "absent"
    if feed_pid is not None:
        feed_status = "ok" if cap_age <= 90 else "pid_alive_writer_dead"

    hb = load(HB)
    hb_age = 99999
    if HB.exists():
        hb_age = int(now - HB.stat().st_mtime)
    hb_updates = int(hb.get("updates") or 0)
    hb_slot = int(hb.get("stream_slot") or 0)
    metrics_ok = False
    if METRICS.exists() and METRICS.stat().st_size > 0:
        try:
            json.loads(METRICS.read_text(encoding="utf-8"))
            metrics_ok = True
        except Exception:
            metrics_ok = False

    prev_state = prev.get("state") or {}
    prev_upd = int(prev_state.get("updates") or 0)
    prev_slot = int(prev_state.get("stream_slot") or 0)
    moving = hb_updates != prev_upd or hb_slot != prev_slot

    state_status = "absent"
    if state_pid is not None:
        if hb_age <= 20 and moving:
            state_status = "ok"
        elif hb_age <= 20 and not metrics_ok:
            state_status = "pid_alive_metrics_dead"
        elif hb_age <= 20:
            state_status = "pid_alive_counters_frozen"
        else:
            state_status = "pid_alive_heartbeat_dead"

    paper_status = "absent"
    if paper_pid is not None:
        if STATS.exists() and STATS.stat().st_size > 0:
            age = int(now - STATS.stat().st_mtime)
            paper_status = "ok" if age <= 30 else "pid_alive_stats_stale"
        else:
            paper_status = "pid_alive_stats_dead"

    doc = {
        "ts": ts,
        "root_free_gb": root_free_gb(),
        "feed": {
            "pid": feed_pid,
            "status": feed_status,
            "newest_cap": cap_path,
            "cap_age_s": cap_age,
            "feed_cap_mtime": cap_mtime,
        },
        "paper": {"pid": paper_pid, "status": paper_status},
        "state": {
            "pid": state_pid,
            "status": state_status,
            "heartbeat_age_s": hb_age,
            "updates": hb_updates,
            "stream_slot": hb_slot,
            "metrics_ok": metrics_ok,
        },
    }
    text = json.dumps(doc, indent=2) + "\n"
    OUT.write_text(text, encoding="utf-8")
    PREV.write_text(text, encoding="utf-8")
    print(json.dumps({
        "ts": ts,
        "feed": feed_status,
        "paper": paper_status,
        "state": state_status,
        "root_free_gb": doc["root_free_gb"],
    }))
    dead = [s for s in (feed_status, paper_status, state_status) if s.startswith("pid_alive_")]
    return 2 if dead else 0


if __name__ == "__main__":
    raise SystemExit(main())
