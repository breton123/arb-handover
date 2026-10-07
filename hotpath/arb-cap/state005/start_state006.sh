#!/bin/bash
# STATE-006 — restart paper_orbit only. feed_live / STATE-005 / expand_loop stay.
set -e
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-cap/state005/state006.py \
  /home/louis/arb-cap/state005/route0_cov.py \
  /home/louis/arb-feed/scripts/paper_orbit.sh \
  /home/louis/arb-feed/src/paper_orbit.c \
  /home/louis/arb-core/src/state003.c \
  /home/louis/arb-core/include/state003.h \
  /home/louis/arb-core/tests/state003.c
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
cd /home/louis/arb-feed/build
cmake --build . --target paper_orbit -j
# Fresh journals so pending/recon/sync match this generation.
: > /home/louis/captures/paper_orbit/pending.jsonl
: > /home/louis/captures/paper_orbit/opp_synced.jsonl
rm -f /home/louis/captures/paper_orbit/recon.bin
python3 -c "import struct,pathlib; p=pathlib.Path('/home/louis/captures/paper_orbit/recon.bin'); p.write_bytes(struct.pack('<IHH',0x36305453,1,0))"
pkill -f "/build/paper_orbit " || true
# Do not pkill expand_loop / state006 by name that matches this script.
if pid=$(pgrep -f "/arb-cap/state005/state006.py"); then
  kill $pid || true
fi
sleep 0.4
export PAPER_SECONDS=43200
export PAPER_UNIV=/home/louis/captures/paper_orbit/liveuniv.bin
export PAPER_SYNC=/home/louis/captures/paper_orbit/sync_state006.bin
export PAPER_PEND=/home/louis/captures/paper_orbit/pending.jsonl
export PAPER_RECON=/home/louis/captures/paper_orbit/recon.bin
export PAPER_AUDIT=/home/louis/captures/paper_orbit/opp_synced.jsonl
nohup /home/louis/arb-feed/scripts/paper_orbit.sh \
  >> /home/louis/captures/paper_orbit/paper_state006.log 2>&1 &
echo "paper_orbit pid $!"
export STATE006_FROM_START=1
nohup python3 /home/louis/arb-cap/state005/state006.py \
  >> /home/louis/captures/paper_orbit/state006.log 2>&1 &
echo "state006 pid $!"
python3 /home/louis/arb-cap/state005/route0_cov.py \
  || true
sleep 1
pgrep -af paper_orbit || true
pgrep -c feed_live
pgrep -af "state005_n|state005/capture|expand_loop|state006.py" || true
tail -n 8 /home/louis/captures/paper_orbit/paper_state006.log || true
