#!/bin/bash
# Rebuild + restart paper_orbit. KEEP journals. Do not touch feed_live.
set -e
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-feed/src/paper_orbit.c \
  /home/louis/arb-feed/CMakeLists.txt \
  /home/louis/arb-core/src/frame.c \
  /home/louis/arb-core/include/frame.h \
  /home/louis/arb-feed/scripts/paper_orbit.sh
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
cd /home/louis/arb-feed
cmake -S . -B build >/tmp/paper_orbit_cmake.log 2>&1
cmake --build build --target paper_orbit -j
pkill -f "/build/paper_orbit " || true
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
sleep 1
pgrep -af paper_orbit || true
pgrep -c feed_live || true
pgrep -af "state006.py|oneshot_live.py|trigger_outcomes.py" || true
echo "journals kept"
wc -l /home/louis/captures/paper_orbit/opp_synced.jsonl \
  /home/louis/captures/paper_orbit/pending.jsonl || true
tail -n 6 /home/louis/captures/paper_orbit/paper_state006.log || true
