#!/bin/bash
set -e
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-cap/state005/rebuild_univ.py \
  /home/louis/arb-cap/state005/STATE005.md
cp -a /home/louis/captures/paper_orbit/liveuniv.bin \
  /home/louis/captures/paper_orbit/liveuniv.bin.bak-pre-state005
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
python3 /home/louis/arb-cap/state005/rebuild_univ.py \
  /home/louis/captures/paper_orbit/liveuniv.json \
  /home/louis/captures/paper_orbit/liveuniv.bin
cd /home/louis/arb-feed/build
cmake --build . --target paper_orbit -j
pkill -f "/build/paper_orbit " || true
sleep 0.3
export PAPER_SECONDS=43200
export PAPER_UNIV=/home/louis/captures/paper_orbit/liveuniv.bin
export PAPER_SYNC=/home/louis/captures/paper_orbit/sync_state005.bin
nohup /home/louis/arb-feed/scripts/paper_orbit.sh \
  >> /home/louis/captures/paper_orbit/paper_state005.log 2>&1 &
echo "paper_orbit pid $!"
sleep 1
pgrep -af paper_orbit || true
pgrep -c feed_live
pgrep -af "state005_n|state005/capture" || true
tail -n 8 /home/louis/captures/paper_orbit/paper_state005.log || true
