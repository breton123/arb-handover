#!/bin/bash
# Next FRAMED universe generation. Restarts paper_orbit + state006 only.
# Leaves ONESHOT#6, racer, feed_live alone.
set -euo pipefail
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-core/include/universe.h \
  /home/louis/arb-core/src/universe.c \
  /home/louis/arb-core/tests/route0_orient.c \
  /home/louis/arb-feed/src/paper_orbit.c \
  /home/louis/arb-cap/live001.py \
  /home/louis/arb-cap/state005/framed_rank.py \
  /home/louis/arb-cap/state005/expand_framed.py \
  /home/louis/arb-cap/state005/state006.py \
  /home/louis/arb-feed/scripts/paper_orbit.sh
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
cd /home/louis/arb-core
cmake -S . -B build >/tmp/core_fam5_cmake.log 2>&1
cmake --build build --target route0_orient
./build/route0_orient
cd /home/louis/arb-feed
cmake -S . -B build >/tmp/feed_expand_cmake.log 2>&1
cmake --build build --target paper_orbit
pkill -f "/home/louis/arb-cap/state005/expand_loop.sh" || true
export UNIV_GEN_LOCKED=1
python3 /home/louis/arb-cap/state005/framed_rank.py
cp -a /home/louis/captures/paper_orbit/liveuniv.bin \
  /home/louis/captures/paper_orbit/liveuniv.bin.bak-pre-framed
python3 /home/louis/arb-cap/state005/expand_framed.py
python3 - <<'PY'
import os, signal
needles = (b"/build/paper_orbit ", b"state005/state006.py")
for pid in os.listdir("/proc"):
    if not pid.isdigit():
        continue
    try:
        cmd = open(f"/proc/{pid}/cmdline", "rb").read()
    except OSError:
        continue
    if any(n in cmd for n in needles):
        os.kill(int(pid), signal.SIGTERM)
        print("stopped", pid)
PY
sleep 1
: > /home/louis/captures/paper_orbit/pending.jsonl
python3 -c "import struct,pathlib; pathlib.Path('/home/louis/captures/paper_orbit/recon.bin').write_bytes(__import__('struct').pack('<IHH',0x36305453,1,0))"
export PAPER_SECONDS=43200
export PAPER_UNIV=/home/louis/captures/paper_orbit/liveuniv.bin
export PAPER_SYNC=/home/louis/captures/paper_orbit/sync_state006.bin
export PAPER_PEND=/home/louis/captures/paper_orbit/pending.jsonl
export PAPER_RECON=/home/louis/captures/paper_orbit/recon.bin
nohup /home/louis/arb-feed/scripts/paper_orbit.sh \
  >> /home/louis/captures/paper_orbit/paper_state006.log 2>&1 < /dev/null &
echo "paper $!"
sleep 1
nohup python3 /home/louis/arb-cap/state005/state006.py \
  >> /home/louis/captures/paper_orbit/state006.log 2>&1 < /dev/null &
echo "state006 $!"
sleep 4
echo "=== paper ==="
tail -n 6 /home/louis/captures/paper_orbit/paper_state006.log
echo "=== oneshot (must stay) ==="
test -e /home/louis/arb-cap/oneshot/ARMED && echo ARMED || echo ARMED_MISSING
pgrep -af "oneshot_live.py|exec_swqos_racer|feed_live|paper_orbit|state006.py" | grep -v grep || true
python3 -c "import json; j=json.load(open('/home/louis/captures/paper_orbit/liveuniv.json')); print('univ', {k:j.get(k) for k in ('gen','n','n_dlmm','n_pump','n_cpmm','added_dlmm')})"
