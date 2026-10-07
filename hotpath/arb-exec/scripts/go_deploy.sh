#!/usr/bin/env bash
# Frankfurt one-liner after funding:
#   ~/TheMoneyMaker/arb-exec/scripts/go_deploy.sh all
# Or step by step: preflight | deploy | verify | simulate | walk
set -euo pipefail

export PATH="${HOME}/.local/share/solana/install/active_release/bin:${PATH}"

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
cd "${REPO}"

if [[ -f "${HOME}/.arb-smoke.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "${HOME}/.arb-smoke.env"
  set +a
fi
if [[ -f "${REPO}/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "${REPO}/.env"
  set +a
fi

SO_DST="${REPO}/arb-exec/program/target/deploy/route0.so"
SO_SRC="${HOME}/arb-exec-live/program/target/deploy/route0.so"
if [[ ! -f "${SO_DST}" && -f "${SO_SRC}" ]]; then
  mkdir -p "$(dirname "${SO_DST}")"
  cp -f "${SO_SRC}" "${SO_DST}"
  echo "staged ${SO_DST} from arb-exec-live"
fi

exec python3 "${HERE}/go_deploy.py" "${1:-preflight}"
