#!/usr/bin/env bash
# Wrapper so cron can source orbitflare.env then run the Python health check.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/orbitflare.env"
export CAPTURE_DIR PREFIX
exec python3 "${HERE}/process_health.py"
