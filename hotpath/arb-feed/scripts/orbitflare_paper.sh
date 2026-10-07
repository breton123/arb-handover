#!/usr/bin/env bash
# Paper-mode OrbitFlare capture. UDP shreds → FEEDCAP1 files. No send. No hot loop.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/orbitflare.env"

if [[ "${PAPER}" != "1" ]]; then
  echo "PAPER=0 — refuse. This script is capture-only."
  exit 1
fi

FEED_LIVE="${HERE}/../build/feed_live"
if [[ ! -x "${FEED_LIVE}" ]]; then
  FEED_LIVE="${HOME}/arb-feed/build/feed_live"
fi
if [[ ! -x "${FEED_LIVE}" ]]; then
  echo "feed_live missing"
  exit 1
fi

bash "${HERE}/orbitflare_prep.sh"

echo "PAPER MODE  capture only"
echo "  bind ${BIND_IP}:${UDP_PORT}  out ${CAPTURE_DIR}"
echo "  cpu ${CPU} rec ${REC_CPU} rcvbuf ${RCVBUF}"
echo "  SIGINT flushes. Never sendTransaction."

exec "${FEED_LIVE}" \
  --bind "${BIND_IP}" \
  --port "${UDP_PORT}" \
  --out "${CAPTURE_DIR}" \
  --prefix "${PREFIX}" \
  --rotate-bytes "${ROTATE_BYTES}" \
  --min-gb "${MIN_GB}" \
  --cpu "${CPU}" \
  --rec-cpu "${REC_CPU}" \
  --rcvbuf "${RCVBUF}" \
  --mlock
