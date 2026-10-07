#!/usr/bin/env bash
# Observe-only Flowra probe. No bundles. No secrets on stdout.
set -euo pipefail
export RUST_LOG=off
export RUST_BACKTRACE=0
HERE="$(cd "$(dirname "$0")" && pwd)"
if [[ -f "${HOME}/.flowra.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "${HOME}/.flowra.env"
  set +a
fi
: "${FLOWRA_DURATION_SEC:=600}"
: "${FLOWRA_CAPTURE_DIR:=${HOME}/captures/flowra}"
: "${FLOWRA_ENDPOINT:=https://frankfurt.mainnet.blockengine.flowra.wtf}"
mkdir -p "${FLOWRA_CAPTURE_DIR}"
BIN="${HERE}/target/release/flowra_probe"
if [[ ! -x "${BIN}" ]]; then
  echo "flowra_probe: building release" >&2
  (cd "${HERE}" && cargo build --release --offline 2>/dev/null || cargo build --release)
fi
exec "${BIN}"
