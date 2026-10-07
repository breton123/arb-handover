#!/bin/bash
# ONESHOT #1. One send, then disarm. Never sends a second time.
set -euo pipefail
export PATH="${HOME}/.cargo/bin:${HOME}/.local/share/solana/install/active_release/bin:${PATH}"
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-exec/scripts/oneshot_live.py \
  /home/louis/arb-exec/tests/exec_swqos_racer.c \
  /home/louis/arb-exec/CMakeLists.txt
cd /home/louis/arb-exec
cmake -S . -B build >/tmp/oneshot_cmake.log 2>&1
cmake --build build --target exec_swqos_racer
test -S /home/louis/arb-cap/oneshot/swqos.sock || {
  echo "racer socket missing — start exec_swqos_racer first"
  exit 1
}
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
if [[ -f /home/louis/.arb-swqos.env ]]; then
  # shellcheck disable=SC1091
  . /home/louis/.arb-swqos.env
fi
set +a
export FUNDED=0
mkdir -p /home/louis/arb-cap/oneshot
exec 9>/home/louis/arb-cap/oneshot/ONESHOT.lock
if ! flock -n 9; then
  echo "oneshot already running"
  exit 75
fi
python3 /home/louis/arb-exec/scripts/oneshot_live.py
