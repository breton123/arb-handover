#!/bin/bash
# TRACK B — compile a new universe generation and atomically swap into paper_orbit.
# Does not touch feed_live, state005_n, or capture.py.
set -e
LOCK=/home/louis/captures/paper_orbit/UNIV_GEN.lock
mkdir -p "$(dirname "${LOCK}")"
exec 9>>"${LOCK}"
if ! flock -n 9; then
  echo "UNIV_GEN busy — skip swap"
  exit 0
fi
export UNIV_GEN_LOCKED=1
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-cap/state005/expand_univ.py \
  /home/louis/arb-cap/state005/rebuild_univ.py \
  /home/louis/arb-cap/state005/univ_gen_lock.py \
  /home/louis/arb-cap/state005/prove_money.py
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
cp -a /home/louis/captures/paper_orbit/liveuniv.bin \
  /home/louis/captures/paper_orbit/liveuniv.bin.bak-pre-expand
python3 /home/louis/arb-cap/state005/expand_univ.py
# Restart only the searcher so it maps the new immutable generation.
pkill -f "/build/paper_orbit " || true
sleep 0.4
export PAPER_SECONDS=43200
export PAPER_UNIV=/home/louis/captures/paper_orbit/liveuniv.bin
export PAPER_SYNC=/home/louis/captures/paper_orbit/sync_state006.bin
export PAPER_PEND=/home/louis/captures/paper_orbit/pending.jsonl
export PAPER_RECON=/home/louis/captures/paper_orbit/recon.bin
: > /home/louis/captures/paper_orbit/pending.jsonl
rm -f /home/louis/captures/paper_orbit/recon.bin
python3 -c "import struct,pathlib; pathlib.Path('/home/louis/captures/paper_orbit/recon.bin').write_bytes(__import__('struct').pack('<IHH',0x36305453,1,0))"
if pid=$(pgrep -f "python3 /home/louis/arb-cap/state005/state006.py"); then
  kill $pid || true
fi
nohup /home/louis/arb-feed/scripts/paper_orbit.sh \
  >> /home/louis/captures/paper_orbit/paper_state006.log 2>&1 &
echo "paper_orbit pid $!"
export STATE006_FROM_START=1
nohup python3 /home/louis/arb-cap/state005/state006.py \
  >> /home/louis/captures/paper_orbit/state006.log 2>&1 &
echo "state006 pid $!"
sleep 1
pgrep -af paper_orbit || true
pgrep -c feed_live
pgrep -af "state005_n|state005/capture" || true
python3 -c "import json; print(json.load(open('/home/louis/captures/paper_orbit/COVERAGE.json'))['coverage_seen_n'])"
tail -n 12 /home/louis/captures/paper_orbit/paper_state006.log || true
