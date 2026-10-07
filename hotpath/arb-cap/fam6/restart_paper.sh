#!/usr/bin/env bash
# Rebuild and restart PAPER only. Does not touch oneshot, state007, rabbit, OUR_EXEC.
set -euo pipefail
PAPER_PID="$(pgrep -f '/home/louis/arb-feed/build/paper_orbit' | head -n1 || true)"
if [[ -n "${PAPER_PID}" ]]; then
  kill "${PAPER_PID}" || true
  sleep 1
  if kill -0 "${PAPER_PID}" 2>/dev/null; then
    kill -9 "${PAPER_PID}" || true
  fi
  echo "stopped_paper ${PAPER_PID}"
fi
cmake --build /home/louis/arb-feed/build --target paper_orbit -j"$(nproc)"
export PAPER_SECONDS=43200
export PAPER_SYNC="${HOME}/captures/paper_orbit/sync_state006.bin"
nohup /home/louis/arb-feed/scripts/paper_orbit.sh \
  >/home/louis/captures/paper_orbit/paper_hops.log 2>&1 &
echo "PAPER_RESTART pid=$!"
sleep 1
pgrep -a -f '/home/louis/arb-feed/build/paper_orbit' || true
pgrep -a -f state007.py || true
pgrep -a -f rabbit001.py || true
pgrep -a -f oneshot_live.py || true
