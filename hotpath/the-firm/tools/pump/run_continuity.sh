#!/bin/bash
set -e
cd /home/louis/the-firm
tar xf /tmp/cont1.tar
make -j"$(nproc)" build/shred_continuity
echo MISSING_JSONL
python3 tools/pump/walk_sigs.py run/ab1/walk97.jsonl run/ab1/missing1068.jsonl
wc -l run/ab1/missing1068.jsonl
CAP=/data/bsc/captures/freeze-live8h-20260927/orbitflare-20260927-040209.cap
echo CONTINUITY
./build/shred_continuity --cap "$CAP" --probes run/ab1/missing1068.jsonl \
    > run/ab1/continuity_slots.jsonl
ps -p 3337378 -o pid=
