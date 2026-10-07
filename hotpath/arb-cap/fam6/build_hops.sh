#!/usr/bin/env bash
set -euo pipefail
export PATH="$HOME/.cargo/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH"
SRC=/home/louis/arb-exec/program_hops
DST=/home/louis/arb-exec-live/program_hops
rm -rf "$DST"
cp -a "$SRC" "$DST"
if [ -d /home/louis/arb-exec-live/program/.cargo ]; then
  cp -a /home/louis/arb-exec-live/program/.cargo "$DST/"
fi
cd "$DST"
cargo-build-sbf
ls -l target/deploy/hops.so
python3 - <<'PY'
from pathlib import Path
p = Path("/home/louis/arb-exec-live/program_hops/target/deploy/hops.so")
print("hops_so_bytes", p.stat().st_size)
# rent-exempt ≈ 2 years * 6960 lamports/byte + metadata
n = p.stat().st_size + 45
print("programdata_rent_est_sol", n * 13920 / 1e9)
print("deploy_peak_2x_est_sol", 2 * n * 13920 / 1e9)
PY
