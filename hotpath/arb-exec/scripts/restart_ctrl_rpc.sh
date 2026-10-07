#!/bin/bash
# Restart control-plane RPC users only. Leave paper_orbit, feed_live, racer.
set -euo pipefail
python3 - <<'PY'
import os, signal
needles = (b"state005/state006.py", b"scripts/oneshot_live.py", b"run_oneshot.sh")
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
sleep 1.2
rm -f /home/louis/arb-cap/oneshot/ARMED /home/louis/arb-cap/oneshot/ONESHOT.lock
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
nohup python3 /home/louis/arb-cap/state005/state006.py \
  >> /home/louis/captures/paper_orbit/state006.log 2>&1 < /dev/null &
echo "state006 $!"
sleep 1
nohup bash /home/louis/arb-exec/scripts/run_oneshot.sh \
  >> /home/louis/arb-cap/oneshot/oneshot6.log 2>&1 < /dev/null &
echo "oneshot $!"
sleep 5
echo "=== state ==="
tail -n 10 /home/louis/captures/paper_orbit/state006.log
echo "=== oneshot ==="
tail -n 12 /home/louis/arb-cap/oneshot/oneshot6.log
echo "=== armed/racer ==="
ls -l /home/louis/arb-cap/oneshot/ARMED /home/louis/arb-cap/oneshot/swqos.sock || true
pgrep -c -f state005/state006.py || true
pgrep -c -f scripts/oneshot_live.py || true
pgrep -c feed_live || true
