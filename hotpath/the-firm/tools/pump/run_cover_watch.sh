#!/bin/bash
set -euo pipefail
# Bounded cover replay: copy recent rotations off the live capture disk,
# snapshot, index that copy, replay. Does not bind :20001 or kill the live pid.
ROOT=${ROOT:-/home/louis/the-firm}
SRCDIR=${SRCDIR:-/data/bsc/captures/orbitflare}
SPAN=${SPAN:-$ROOT/run/cover_span}
LOG=${LOG:-$ROOT/run/cover_watch.log}
POOLS=${POOLS:-$ROOT/run/cover_snap.jsonl}
KEYS=${KEYS:-$ROOT/run/cover_keys.txt}
LUTS=${LUTS:-$ROOT/run/lut_learned.jsonl}
NFILES=${NFILES:-8}
BIN=$ROOT/build/live_state_001
IDX=$ROOT/build/cap_index

exec > >(tee -a "$LOG") 2>&1
echo "cover_watch_start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ ! -x "$BIN" ] || [ ! -x "$IDX" ]; then
  echo "missing binaries" >&2
  exit 1
fi
python3 -c "
import json, sys
src, dst = sys.argv[1], sys.argv[2]
n = 0
with open(src) as f, open(dst, 'w') as o:
    for line in f:
        line = line.strip()
        if not line:
            continue
        o.write(json.loads(line)['pool_hex'] + '\n')
        n += 1
print('keys', n, dst)
" "$ROOT/configs/pools/seed.jsonl" "$KEYS"
python3 "$ROOT/tools/anchor/snapshot.py" --provider shyft --pools "$KEYS" --out "$POOLS"
S=$(python3 -c "
import json, sys
s = None
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    v = int(json.loads(line).get('slot', 0))
    s = v if s is None or v < s else s
print(s if s is not None else '')
" "$POOLS")
if [ -z "$S" ]; then
  echo "no snapshot slot" >&2
  exit 1
fi
echo "anchor_min=$S"
rm -rf "$SPAN"
mkdir -p "$SPAN"
# Newest N sealed+current rotations only. Do not index the 401G live tree.
i=0
for f in $(ls -t "$SRCDIR"/orbitflare-*.cap); do
  i=$((i + 1))
  if [ "$i" -gt "$NFILES" ]; then
    break
  fi
  cp -- "$f" "$SPAN/"
  echo "copied $(basename "$f")"
done
set +e
"$IDX" --dir "$SPAN" --anchor "$S"
rc=$?
set -e
if [ "$rc" -eq 3 ]; then
  echo "CAPTURE_COVERS_ANCHOR=false; do not replay"
  echo "cover_watch_done rc=3"
  exit 3
fi
if [ "$rc" -ne 0 ]; then
  echo "cover_watch_done rc=$rc"
  exit "$rc"
fi
"$BIN" --cap-dir "$SPAN" --pools "$POOLS" --luts "$LUTS"
echo "cover_watch_done rc=0 $(date -u +%Y-%m-%dT%H:%M:%SZ)"
