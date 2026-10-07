#!/usr/bin/env bash
# Install minute cron for disk guard + process health. Restart paper feed_live on /data.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/orbitflare.env"

sed -i 's/\r$//' "${HERE}/orbitflare_watch.sh" "${HERE}/process_health.sh" "${HERE}/process_health.py" || true
chmod +x "${HERE}/orbitflare_watch.sh" "${HERE}/process_health.sh" "${HERE}/process_health.py"

LINE1="* * * * * ${HERE}/orbitflare_watch.sh >> /data/bsc/captures/orbitflare/watch.log 2>&1"
LINE2="* * * * * ${HERE}/process_health.sh >> /dev/shm/process_health.log 2>&1"

cur="$(crontab -l 2>/dev/null || true)"
{
  echo "${cur}" | grep -v orbitflare_watch.sh | grep -v process_health.sh || true
  echo "${LINE1}"
  echo "${LINE2}"
} | crontab -
echo "CRON"
crontab -l | grep -E 'orbitflare_watch|process_health'

# Restart capture onto the data volume. Never start paper_orbit or state008.
if pgrep -f "feed_live --prefix ${PREFIX}" >/dev/null; then
  pkill -f "feed_live --prefix ${PREFIX}" || true
  sleep 1
fi
FEED_LIVE="${HERE}/../build/feed_live"
if [[ ! -x "${FEED_LIVE}" ]]; then
  FEED_LIVE="${HOME}/arb-feed/build/feed_live"
fi
mkdir -p "${CAPTURE_DIR}"
nohup "${FEED_LIVE}" \
  --bind "${BIND_IP}" --port "${UDP_PORT}" \
  --out "${CAPTURE_DIR}" --prefix "${PREFIX}" \
  --rotate-bytes "${ROTATE_BYTES}" --min-gb "${MIN_GB}" \
  --cpu "${CPU}" --rec-cpu "${REC_CPU}" --rcvbuf "${RCVBUF}" --mlock \
  >> "${CAPTURE_DIR}/feed_live.log" 2>&1 < /dev/null &
echo "FEED $!"
sleep 1
pgrep -a feed_live || echo "FEED_MISSING"
"${HERE}/orbitflare_watch.sh" || true
python3 "${HERE}/process_health.py" || true
echo INSTALL_OK
