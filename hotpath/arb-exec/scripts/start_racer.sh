#!/bin/bash
# Keep SWQOS READY pool open. Do not open/close per opportunity.
set -euo pipefail
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-exec/tests/exec_swqos_racer.c \
  /home/louis/arb-exec/CMakeLists.txt
cd /home/louis/arb-exec
cmake -S . -B build >/tmp/racer_cmake.log 2>&1
cmake --build build --target exec_swqos_racer
python3 - <<'PY'
import os, signal
for pid in os.listdir("/proc"):
    if not pid.isdigit():
        continue
    try:
        cmd = open(f"/proc/{pid}/cmdline", "rb").read()
    except OSError:
        continue
    if b"exec_swqos_racer" in cmd:
        os.kill(int(pid), signal.SIGTERM)
        print("stopped", pid)
PY
sleep 0.3
rm -f /home/louis/arb-cap/oneshot/swqos.sock
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
if [[ -f /home/louis/.arb-swqos.env ]]; then
  # shellcheck disable=SC1091
  . /home/louis/.arb-swqos.env
fi
set +a
mkdir -p /home/louis/arb-cap/oneshot
nohup /home/louis/arb-exec/build/exec_swqos_racer \
  >> /home/louis/arb-cap/oneshot/racer.log 2>&1 < /dev/null &
echo "racer pid $!"
for i in $(seq 1 40); do
  if [[ -S /home/louis/arb-cap/oneshot/swqos.sock ]]; then
    echo "racer sock up"
    tail -n 3 /home/louis/arb-cap/oneshot/racer.log || true
    exit 0
  fi
  sleep 0.15
done
echo "racer sock missing"
tail -n 20 /home/louis/arb-cap/oneshot/racer.log || true
exit 1
