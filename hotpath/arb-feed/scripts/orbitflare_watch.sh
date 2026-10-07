#!/usr/bin/env bash
# Hard retention + root ENOSPC guard. Cron every minute.
# If root free < ROOT_MIN_GB: stop feed_live, delete leftover root caps, prune capture dir.
# If capture dir > RETAIN_MAX_GB or files older than RETAIN_DAYS: delete oldest closed caps.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/orbitflare.env"

ALARM_FILE="${CAPTURE_DIR}/.disk_alarm"
mkdir -p "${CAPTURE_DIR}"

free_gb() {
  local p="$1"
  df -P "${p}" | awk 'NR==2{printf "%d",$4/1024/1024}'
}

root_used_caps() {
  find /home/louis/captures -xdev -type f \( -name 'orbitflare-*.cap' -o -name 'shredstream-*.cap' \) 2>/dev/null || true
}

newest_cap() {
  ls -1t "${CAPTURE_DIR}"/orbitflare-*.cap 2>/dev/null | head -1 || true
}

prune_oldest() {
  local newest="$1"
  local deleted=0
  local f
  # Closed files only: skip the newest writer and anything still growing if we can.
  mapfile -t files < <(ls -1tr "${CAPTURE_DIR}"/orbitflare-*.cap 2>/dev/null || true)
  for f in "${files[@]}"; do
    [[ -n "${f}" ]] || continue
    [[ "${f}" == "${newest}" ]] && continue
    [[ -L "${f}" ]] && continue
    rm -f "${f}"
    deleted=$((deleted + 1))
    local used
    used="$(du -sb "${CAPTURE_DIR}" 2>/dev/null | awk '{print int($1/1024/1024/1024)}')"
    local root_free
    root_free="$(free_gb "${ROOT_FS}")"
    if (( used <= RETAIN_MAX_GB )) && (( root_free >= ROOT_MIN_GB )); then
      break
    fi
  done
  echo "${deleted}"
}

TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
ROOT_FREE="$(free_gb "${ROOT_FS}")"
CAP_FREE="$(free_gb "${CAPTURE_DIR}")"
CAP_USED="$(du -sb "${CAPTURE_DIR}" 2>/dev/null | awk '{print int($1/1024/1024/1024)}')"
NEWEST="$(newest_cap)"

echo "${TS}  root=${ROOT_FREE}G  cap_free=${CAP_FREE}G  cap_used=${CAP_USED}G  retain=${RETAIN_MAX_GB}G  newest=$(basename "${NEWEST:-none}")"

stopped=0
if (( ROOT_FREE < ROOT_MIN_GB )); then
  msg="${TS} ALARM root ${ROOT_FREE} GB < ${ROOT_MIN_GB} GB — stop feed_live, prune"
  echo "${msg}" | tee -a "${ALARM_FILE}"
  pkill -f "feed_live --prefix ${PREFIX}" 2>/dev/null || true
  stopped=1
  while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    rm -f "${f}"
  done < <(root_used_caps)
  prune_oldest "${NEWEST}" >/dev/null || true
fi

if (( CAP_USED > RETAIN_MAX_GB )); then
  echo "${TS} RETAIN cap_used ${CAP_USED} GB > ${RETAIN_MAX_GB} GB"
  prune_oldest "${NEWEST}" >/dev/null || true
fi

if [[ -n "${RETAIN_DAYS}" ]]; then
  find "${CAPTURE_DIR}" -maxdepth 1 -type f -name 'orbitflare-*.cap' -mtime "+${RETAIN_DAYS}" ! -path "${NEWEST}" -delete 2>/dev/null || true
fi

if (( CAP_FREE < DISK_ALARM_GB )); then
  msg="${TS} ALARM capture-fs ${CAP_FREE} GB < ${DISK_ALARM_GB} GB — stop feed_live"
  echo "${msg}" | tee -a "${ALARM_FILE}"
  pkill -f "feed_live --prefix ${PREFIX}" 2>/dev/null || true
  stopped=1
fi

if (( stopped == 1 )); then
  exit 2
fi
exit 0
