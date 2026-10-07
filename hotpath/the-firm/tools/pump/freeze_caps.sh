#!/bin/bash
set -euo pipefail
SRC=/data/bsc/captures/orbitflare
FR=/data/bsc/captures/freeze-live8h-20260927
LIST=/home/louis/the-firm/run/cap_list.txt
mkdir -p "$FR"
# Live process started 04:02 local on 2026-09-27.
find "$SRC" -maxdepth 1 -name 'orbitflare-20260927-*.cap' | sort | while read -r f; do
  base=$(basename "$f")
  # Filenames are YYYYMMDD-HHMMSS. Keep from 040207 onward.
  ts=${base#orbitflare-20260927-}
  ts=${ts%.cap}
  if [ "$ts" \< "040207" ]; then
    continue
  fi
  ln -f "$f" "$FR/$base"
done
ls "$FR" | sort | awk -v d="$FR" '{print d "/" $0}' > "$LIST"
echo "files $(wc -l < "$LIST")"
du -sh "$FR"
head -n 2 "$LIST"
tail -n 2 "$LIST"
