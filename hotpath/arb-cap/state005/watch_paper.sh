#!/bin/bash
end=$((SECONDS + ${1:-300}))
echo "t decoded known decide opp"
while [ "$SECONDS" -lt "$end" ]; do
  line=$(grep "PAPER-ORBIT  up=" /home/louis/captures/paper_orbit/paper_state005.log | tail -n 1)
  echo "$(date -u +%H:%M:%S) $line"
  sleep 15
done
tail -n 3 /home/louis/captures/paper_orbit/paper_state005.log
python3 /home/louis/arb-cap/state005/overlap.py \
  /home/louis/captures/state005/seen_n.jsonl \
  /home/louis/captures/paper_orbit/liveuniv.json
python3 /home/louis/arb-cap/state005/gate.py | head -n 2
