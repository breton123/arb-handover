#!/bin/bash
set -e
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-cap/state005/capture.py \
  /home/louis/arb-cap/state005/reclassify.py
pkill -f "state005/capture.py" || true
sleep 0.3
python3 /home/louis/arb-cap/state005/reclassify.py \
  /home/louis/captures/state005 \
  /home/louis/arb-core/build/state005
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
nohup python3 /home/louis/arb-cap/state005/capture.py \
  /home/louis/captures/state005/seen_n.jsonl \
  /home/louis/captures/state005 \
  /home/louis/arb-core/build/state005 \
  >> /home/louis/captures/state005/capture.log 2>&1 &
echo "capture pid $!"
sleep 1
pgrep -af "state005/capture.py"
pgrep -c feed_live
