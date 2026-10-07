#!/bin/bash
set -e
OUT=/home/louis/captures/state005
mkdir -p "$OUT"
python3 /home/louis/arb-cap/state005/crlf.py \
  /home/louis/arb-cap/record_dlmm.py \
  /home/louis/arb-cap/state005/capture.py \
  /home/louis/arb-cap/state004/decomp.py >/dev/null || true
if pgrep -f "/build/state005_n " >/dev/null; then
  echo "state005_n already running"
else
  nohup /home/louis/arb-feed/build/state005_n \
    --dir /home/louis/captures/orbitflare \
    --out "$OUT/seen_n.jsonl" \
    >> "$OUT/n.log" 2>&1 &
  echo "started state005_n pid $!"
fi
if pgrep -f "state005/capture.py" >/dev/null; then
  echo "capture.py already running"
else
  set -a
  # shellcheck disable=SC1091
  . /home/louis/.arb-smoke.env
  set +a
  nohup python3 /home/louis/arb-cap/state005/capture.py \
    "$OUT/seen_n.jsonl" \
    "$OUT" \
    /home/louis/arb-core/build/state005 \
    >> "$OUT/capture.log" 2>&1 &
  echo "started capture.py pid $!"
fi
sleep 1
pgrep -a state005 || true
pgrep -af "state005/capture.py" || true
echo "feed_live $(pgrep -c feed_live) (must stay 1+)"
tail -n 5 "$OUT/n.log" || true
tail -n 5 "$OUT/capture.log" || true
