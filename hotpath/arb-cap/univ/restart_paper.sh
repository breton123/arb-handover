#!/usr/bin/env bash
set -eu
pids=$(pgrep -f '/home/louis/arb-feed/.*/paper_orbit' || true)
if [ -n "$pids" ]; then
  kill $pids 2>/dev/null || true
fi
sleep 2
pids=$(pgrep -f '/home/louis/arb-feed/.*/paper_orbit' || true)
if [ -n "$pids" ]; then
  kill -9 $pids 2>/dev/null || true
fi
sleep 1
export PAPER_SECONDS=43200
export PAPER_SYNC="${HOME}/captures/paper_orbit/sync_state006.bin"
nohup /home/louis/arb-feed/scripts/paper_orbit.sh \
  >/home/louis/captures/paper_orbit/paper_univgen.log 2>&1 &
sleep 3
pgrep -a paper_orbit || echo paper_missing
grep -E 'UNIV-GEN|liveuniv load|FAIL' /home/louis/captures/paper_orbit/paper_univgen.log | tail -n 8 || true
echo oneshot=$(ls /home/louis/arb-cap/oneshot/DISARMED 2>/dev/null || echo missing)
pgrep -a state007 | head -n 1 || true
pgrep -a rabbit001 | head -n 1 || true
