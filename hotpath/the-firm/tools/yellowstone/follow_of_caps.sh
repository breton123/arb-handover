#!/bin/bash
# Follow the newest OF rotate without binding UDP. Restarts live_truth
# when feed_live opens a new .cap. Candidate jsonl is append-only.
set -euo pipefail
ROOT="${HOME}/the-firm"
OUTDIR="/data/bsc/captures/live_truth"
LUT="${ROOT}/configs/luts/warm.jsonl"
YS="${OUTDIR}/ys.jsonl"
CANDS="${OUTDIR}/pamm_cands.jsonl"
mkdir -p "$OUTDIR"
touch "$YS"
last=""
pid=""
while true; do
    cap=$(ls -1t /data/bsc/captures/orbitflare/orbitflare-*.cap 2>/dev/null | head -1 || true)
    alive=0
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        alive=1
    fi
    if [ -n "$cap" ] && [ "$alive" -eq 0 ]; then
        last=$cap
        echo "follow_of_caps  cap=$cap" >>"$OUTDIR/live_truth.log"
        if [ ! -f "$LUT" ]; then
            mkdir -p "$(dirname "$LUT")"
            : >"$LUT"
        fi
        "${ROOT}/build/live_truth" \
            --cap "$cap" \
            --ys "$YS" \
            --luts "$LUT" \
            --cands "$CANDS" \
            --follow >>"$OUTDIR/live_truth.log" 2>&1 &
        pid=$!
    fi
    sleep 2
done
