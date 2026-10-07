#!/bin/bash
# Start YS subscriber beside already-running OF + AUTH. Do not bind :20001.
set -euo pipefail
ROOT="${HOME}/the-firm"
OUTDIR="/data/bsc/captures/live_truth"
mkdir -p "$OUTDIR"
touch "$OUTDIR/ys.jsonl"
if pgrep -f 'ys_pamm_sub.py' >/dev/null; then
    echo "ys_pamm_sub already running"
else
    nohup "$ROOT/tools/yellowstone/start_ys_pamm.sh" "$OUTDIR/ys.jsonl" \
        >"$OUTDIR/ys_sub.log" 2>&1 &
    echo "ys_pamm_sub started pid=$!"
fi
CAP=$(ls -1t /data/bsc/captures/orbitflare/orbitflare-*.cap | head -1)
echo "OF cap=$CAP"
echo "AUTH=state008 (do not start a second UDP feed)"
echo "LUT=$ROOT/configs/luts/warm.jsonl"
exec "$ROOT/build/live_truth" \
    --cap "$CAP" \
    --ys "$OUTDIR/ys.jsonl" \
    --luts "$ROOT/configs/luts/warm.jsonl" \
    --cands "$OUTDIR/pamm_cands.jsonl" \
    --follow
