#!/bin/bash
# Poll until 10 clean + >=1 multi-bin walk, or timeout.
end=$((SECONDS + ${1:-900}))
while [ "$SECONDS" -lt "$end" ]; do
  out=$(python3 /home/louis/arb-cap/state005/gate.py)
  echo "$(date -u +%H:%M:%S) $out" | tr '\n' ' '
  echo
  if echo "$out" | grep -q "GATE OPEN"; then
    exit 0
  fi
  sleep 20
done
exit 2
