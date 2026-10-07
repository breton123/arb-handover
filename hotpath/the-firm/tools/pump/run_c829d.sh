#!/bin/bash
set -e
cd /home/louis/the-firm
tar xf /tmp/c829d.tar
rm -f build/apply.o build/live.o build/tx_pumpstate.o build/oracle.o \
      build/oracle_case.o build/wire_pipeline.o build/test_apply \
      build/live_state_001
make -j"$(nproc)" build/test_apply build/live_state_001
./build/test_apply
CAP=/data/bsc/captures/freeze-live8h-20260927/orbitflare-20260927-040209.cap
SIG=c829b8b5704a003eeb68d43c1c7e3e5b502211c7adf8e06e1acc3906bf6810f55a08883cb0bd9ba55d7790957eddbb953abffd20d2d4571f276cfbb690c44309
echo TRACE
./build/live_state_001 --cap "$CAP" --pools configs/pools/seed.jsonl \
    --lut-frozen run/ab1/lut_sealed.jsonl --trace-sig "$SIG" \
    > run/ab1/trace3.out 2> run/ab1/trace3.err || true
echo TRACE_DUMP
grep -E 'pumpstate=|apply code=|overlay_|fail_step|overlay_pool|ix\[|src_sys|dst_sys|pool_id|src_token|dst_token|vault_|fee_|ain=|TOKEN_CLOSE|ATA_CREATE|PUMP_SELL' run/ab1/trace3.err | head -80
ps -p 3337378 -o pid=
