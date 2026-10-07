#!/usr/bin/env bash
set -euo pipefail
export PATH="${HOME}/.local/share/solana/install/active_release/bin:${PATH}"

mkdir -p \
  "${HOME}/arb-exec/scripts" \
  "${HOME}/arb-exec/.deploy" \
  "${HOME}/arb-exec/program/src" \
  "${HOME}/arb-exec/program/target/deploy" \
  "${HOME}/arb-feed/scripts" \
  "${HOME}/arb-cap/tomorrow" \
  "${HOME}/captures/orbitflare"

if [[ -f "${HOME}/arb-exec-live/program/target/deploy/route0.so" ]]; then
  cp -f "${HOME}/arb-exec-live/program/target/deploy/route0.so" \
        "${HOME}/arb-exec/program/target/deploy/route0.so"
  echo "staged route0.so $(wc -c < "${HOME}/arb-exec/program/target/deploy/route0.so") B"
fi

chmod 600 "${HOME}/arb-exec/.deploy/program-v3.json" 2>/dev/null || true
chmod +x \
  "${HOME}/arb-exec/scripts/go_deploy.sh" \
  "${HOME}/arb-feed/scripts/orbitflare_prep.sh" \
  "${HOME}/arb-feed/scripts/orbitflare_watch.sh" \
  "${HOME}/arb-feed/scripts/orbitflare_paper.sh"

cd "${HOME}/arb-feed"
cmake --build build --target feed_live feed001

echo "=== feed001 ==="
./build/feed001 | tail -20

echo "=== go_deploy preflight ==="
python3 "${HOME}/arb-exec/scripts/go_deploy.py" preflight; echo "preflight_exit=$?"

echo "=== orbitflare prep ==="
bash "${HOME}/arb-feed/scripts/orbitflare_prep.sh"; echo "prep_exit=$?"
