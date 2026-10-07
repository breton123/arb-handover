#!/bin/bash
set -e
cd /home/louis/the-firm
tar xf /tmp/walk97.tar
rm -f build/live.o build/live_state_001
make -j"$(nproc)" build/live_state_001
CAP=/data/bsc/captures/freeze-live8h-20260927/orbitflare-20260927-040209.cap
POOL=97e81bb1594ed2cb0757d9c8d65e9a01e6f2290af0b2e8ab3590bab163e5d735
echo WALK_REPLAY
./build/live_state_001 --cap "$CAP" --pools configs/pools/seed.jsonl \
    --lut-frozen run/ab1/lut_sealed.jsonl \
    --walk-pool "$POOL" --walk-from 450882490 --walk-to 450883470 \
    --walk-log run/ab1/walk97.jsonl \
    > run/ab1/walk97.out 2> run/ab1/walk97.err
grep -E 'WALK |LUT_MISS:|Applied:|DIRTY:' run/ab1/walk97.out || true
wc -l run/ab1/walk97.jsonl
echo WALK_JOIN
python3 tools/pump/walk_join.py run/ab1/walk97.jsonl run/ab1/walk97_rpc.json 2500
ps -p 3337378 -o pid=
