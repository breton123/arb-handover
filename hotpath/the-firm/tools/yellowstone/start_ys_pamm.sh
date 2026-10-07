#!/bin/bash
# Frankfurt only. Sources local env files; never echoes them.
set -euo pipefail
set -a
# shellcheck disable=SC1090
[ -f "$HOME/.arb-state007.env" ] && . "$HOME/.arb-state007.env"
# shellcheck disable=SC1090
[ -f "$HOME/.arb-smoke.env" ] && . "$HOME/.arb-smoke.env"
set +a
OUT="${1:-/data/bsc/captures/live_truth/ys.jsonl}"
mkdir -p "$(dirname "$OUT")"
export SHYFT_DIR="${SHYFT_DIR:-$HOME/arb-state/shyft}"
exec python3 "$HOME/the-firm/tools/yellowstone/ys_pamm_sub.py" --out "$OUT"
