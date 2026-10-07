#!/usr/bin/env bash
set -euo pipefail
DIR="${TRIGGER011_DIR:-$HOME/captures/trigger011}"
mkdir -p "$DIR"
set -a
# shellcheck disable=SC1091
. /home/louis/.arb-smoke.env
set +a
for pid in $(pgrep -f "arb-cap/trigger011/alt_plane.py" || true); do
  comm=$(tr '\0' ' ' < "/proc/$pid/cmdline" || true)
  case "$comm" in
    *alt_plane.py*) kill "$pid" || true; echo "stopped alt_plane $pid" ;;
  esac
done
sleep 1
nohup env TRIGGER011_DIR="$DIR" FUNDED=0 \
  python3 /home/louis/arb-cap/trigger011/alt_plane.py \
  >> "$DIR/alt_plane.log" 2>&1 < /dev/null &
echo "alt_plane $! dir=$DIR FUNDED=0"
sleep 1
pgrep -af "python3 /home/louis/arb-cap/trigger011/alt_plane.py" | grep -v pgrep || echo ALT_PLANE_MISSING
tail -5 "$DIR/alt_plane.log" || true
