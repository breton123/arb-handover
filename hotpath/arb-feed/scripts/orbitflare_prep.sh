#!/usr/bin/env bash
# Stage OrbitFlare last-hop capture. Does not start the feed.
# Run on Frankfurt:  bash ~/arb-feed/scripts/orbitflare_prep.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/orbitflare.env"

FEED_LIVE="${HERE}/../build/feed_live"
if [[ ! -x "${FEED_LIVE}" ]]; then
  FEED_LIVE="${HOME}/arb-feed/build/feed_live"
fi

echo "ORBITFLARE PREP"
echo "  public_ip    ${PUBLIC_IP}"
echo "  bind         ${BIND_IP}:${UDP_PORT}"
echo "  capture_dir  ${CAPTURE_DIR}"
echo "  prefix       ${PREFIX}"
echo "  cpu/rec      ${CPU}/${REC_CPU}"
echo "  rcvbuf       ${RCVBUF}"
echo "  min_gb       ${MIN_GB}  alarm=${DISK_ALARM_GB}"
echo "  paper        ${PAPER}"

LIVE_IP="$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1 || true)"
if [[ "${LIVE_IP}" != "${PUBLIC_IP}" ]]; then
  echo "  ! interface IP ${LIVE_IP} != PUBLIC_IP ${PUBLIC_IP}"
  exit 1
fi
echo "  interface    ${LIVE_IP}  OK"

if [[ ! -x "${FEED_LIVE}" ]]; then
  echo "  ! feed_live missing at ${FEED_LIVE}"
  exit 1
fi
echo "  feed_live    ${FEED_LIVE}  OK"

mkdir -p "${CAPTURE_DIR}"
# Never write into the FEEDCAP1 trial files sitting in ~/captures/*.cap
if [[ "${CAPTURE_DIR}" == "/home/louis/captures" ]]; then
  echo "  ! refuse: do not mix OrbitFlare with FEEDCAP1 trial dir"
  exit 1
fi

FREE_GB="$(df -P "${CAPTURE_DIR}" | awk 'NR==2{printf "%d",$4/1024/1024}')"
echo "  disk_free    ${FREE_GB} GB"
if (( FREE_GB < DISK_ALARM_GB )); then
  echo "  ! disk below alarm ${DISK_ALARM_GB} GB"
  exit 1
fi

NPROC="$(nproc)"
if (( CPU >= NPROC || REC_CPU >= NPROC || CPU < 0 || REC_CPU < 0 )); then
  echo "  ! cpu ${CPU} or rec-cpu ${REC_CPU} out of range (nproc=${NPROC})"
  exit 1
fi
echo "  nproc        ${NPROC}  pin ${CPU}/${REC_CPU}  OK"

RMAX="$(sysctl -n net.core.rmem_max)"
echo "  rmem_max     ${RMAX}"
if (( RMAX < RCVBUF )); then
  echo "  ! rmem_max ${RMAX} < rcvbuf ${RCVBUF}"
  echo "    sudo sysctl -w net.core.rmem_max=${RCVBUF}"
  echo "    sudo sysctl -w net.core.rmem_default=${RCVBUF}"
  exit 1
fi

echo
echo "FIREWALL  (sudo password required on this box)"
echo "  sudo ufw allow ${UDP_PORT}/udp comment orbitflare-shreds"
echo "  sudo ufw status | grep ${UDP_PORT} || true"
echo "  # or iptables:"
echo "  sudo iptables -C INPUT -p udp --dport ${UDP_PORT} -j ACCEPT 2>/dev/null \\"
echo "    || sudo iptables -I INPUT -p udp --dport ${UDP_PORT} -j ACCEPT"
if sudo -n true 2>/dev/null; then
  if command -v ufw >/dev/null; then
    sudo ufw allow "${UDP_PORT}/udp" comment orbitflare-shreds || true
    sudo ufw status | grep -E "${UDP_PORT}|Status" || true
  fi
else
  echo "  sudo needs a password — run the two lines above after login"
fi

echo
echo "VENDOR  tell OrbitFlare:"
echo "  dst ${PUBLIC_IP}:${UDP_PORT}  proto UDP  shreds"
echo
echo "PAPER START"
echo "  bash ${HERE}/orbitflare_paper.sh"
echo "DISK WATCH"
echo "  bash ${HERE}/orbitflare_watch.sh"
echo "PREP OK"
