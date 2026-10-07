#!/usr/bin/env python3
"""Single-instance lock for universe generation build/publish.

Advisory flock on UNIV_GEN.lock. Parent shells that already hold the
same file set UNIV_GEN_LOCKED=1 so the Python layer does not nest.
Direct `python3 expand_univ.py` still locks here.

Exit 75 (EX_TEMPFAIL) if the lock is busy and wait is off.
"""
from __future__ import annotations

import fcntl
import os
import sys
import time
from pathlib import Path

LOCK_PATH = Path("/home/louis/captures/paper_orbit/UNIV_GEN.lock")
LOCK_BUSY = 75
_HELD_FD: int | None = None


def acquire(*, wait: bool | None = None) -> int | None:
    """Return the lock fd, or None if a parent already holds it.

    Keeps the fd open for the process lifetime unless release() is called.
    """
    global _HELD_FD
    if os.environ.get("UNIV_GEN_LOCKED") == "1":
        return None
    if wait is None:
        wait = os.environ.get("UNIV_LOCK_WAIT") == "1"
    LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(LOCK_PATH, os.O_CREAT | os.O_RDWR, 0o644)
    flags = fcntl.LOCK_EX if wait else fcntl.LOCK_EX | fcntl.LOCK_NB
    try:
        fcntl.flock(fd, flags)
    except BlockingIOError:
        holder = "unknown"
        try:
            holder = os.pread(fd, 64, 0).decode("ascii", "replace").strip() or holder
        except OSError:
            pass
        os.close(fd)
        print(f"UNIV_GEN locked by {holder} — skip", flush=True)
        raise SystemExit(LOCK_BUSY) from None
    os.lseek(fd, 0, os.SEEK_SET)
    os.ftruncate(fd, 0)
    os.write(fd, f"{os.getpid()}\n".encode("ascii"))
    os.fsync(fd)
    _HELD_FD = fd
    print(f"UNIV_GEN lock acquired pid={os.getpid()}", flush=True)
    return fd


def release() -> None:
    global _HELD_FD
    fd = _HELD_FD
    _HELD_FD = None
    if fd is None:
        return
    try:
        fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "--hold":
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
        acquire(wait=False)
        time.sleep(secs)
        release()
        return 0
    acquire(wait=False)
    release()
    print("UNIV_GEN lock ok", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
