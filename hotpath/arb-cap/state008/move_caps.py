#!/usr/bin/env python3
"""Move closed orbitflare/shredstream caps onto /data/bsc. Live writers stay on root."""
from __future__ import annotations

import os
import shutil
from pathlib import Path

SRC_OF = Path("/home/louis/captures/orbitflare")
SRC_CAP = Path("/home/louis/captures")
DST_OF = Path("/data/bsc/captures/orbitflare")
DST_SH = Path("/data/bsc/captures/shredstream")
LIVE = "orbitflare-20260925-213432.cap"  # updated at runtime


def live_cap() -> str | None:
    newest = None
    newest_m = -1.0
    if not SRC_OF.is_dir():
        return None
    for p in SRC_OF.glob("orbitflare-*.cap"):
        if p.is_symlink():
            continue
        m = p.stat().st_mtime
        if m > newest_m:
            newest_m = m
            newest = p.name
    return newest


def move_file(src: Path, dst_dir: Path) -> None:
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / src.name
    if dst.exists():
        src.unlink(missing_ok=True)
        if not src.exists():
            src.symlink_to(dst)
        return
    shutil.move(str(src), str(dst))
    if not src.exists():
        src.symlink_to(dst)


def main() -> int:
    live = live_cap()
    n_of = n_sh = 0
    if SRC_OF.is_dir():
        for p in sorted(SRC_OF.glob("orbitflare-*.cap")):
            if p.is_symlink() or p.name == live:
                continue
            move_file(p, DST_OF)
            n_of += 1
            if n_of % 20 == 0:
                print(f"orbitflare moved {n_of}", flush=True)
    for p in sorted(SRC_CAP.glob("shredstream-*.cap")):
        if p.is_symlink():
            continue
        move_file(p, DST_SH)
        n_sh += 1
    print(f"MOVE_CAPS  orbitflare={n_of} shredstream={n_sh} live={live}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
