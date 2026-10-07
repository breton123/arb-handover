#!/bin/bash
# Periodic TRACK B. Sleeps between generations. Never touches feed_live / STATE-005.
set -e
while true; do
  sleep 900
  echo "EXPAND-LOOP $(date -u +%FT%TZ)"
  /home/louis/arb-cap/state005/expand_swap.sh || echo "expand_swap failed $?"
done
