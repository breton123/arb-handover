#!/usr/bin/env bash
# Follow OrbitFlare .cap from EOF. Fresh S already loaded. NO SEND. Does not touch feed_live.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=C1091
source "${HERE}/orbitflare.env"

BIN="${HERE}/../build/paper_orbit"
UNIV="${PAPER_UNIV:-${HOME}/captures/paper_orbit/liveuniv.bin}"
SYNC="${PAPER_SYNC:-${HOME}/captures/paper_orbit/sync.bin}"
PEND="${PAPER_PEND:-${HOME}/captures/paper_orbit/pending.jsonl}"
RECON="${PAPER_RECON:-${HOME}/captures/paper_orbit/recon.bin}"
AUDIT="${PAPER_AUDIT:-${HOME}/captures/paper_orbit/opp_synced.jsonl}"
STATS="${PAPER_STATS:-${HOME}/captures/paper_orbit/stats.json}"
FLOWRA_DIR="${FLOWRA_CAPTURE_DIR:-${HOME}/captures/flowra}"
SECS="${PAPER_SECONDS:-1200}"

if [[ ! -x "${BIN}" ]]; then
  echo "paper_orbit missing"
  exit 1
fi
if [[ ! -f "${UNIV}" ]]; then
  echo "liveuniv missing: ${UNIV}"
  exit 1
fi
mkdir -p "$(dirname "${SYNC}")"

echo "PAPER-ORBIT  follow ${CAPTURE_DIR}  univ ${UNIV}  ${SECS}s  NO SEND  STATE-006"
# Do not inherit UNIV_GEN.lock from expand_swap (fd 9).
# Keep this in a subshell so we do not redirect this script's stderr.
{ exec 9>&-; } 2>/dev/null || true
exec "${BIN}" \
  --dir "${CAPTURE_DIR}" \
  --univ "${UNIV}" \
  --sync "${SYNC}" \
  --pend "${PEND}" \
  --recon "${RECON}" \
  --audit "${AUDIT}" \
  --stats "${STATS}" \
  --flowra-dir "${FLOWRA_DIR}" \
  --seconds "${SECS}"
