#!/usr/bin/env bash
set -euo pipefail
export PATH="${HOME}/.cargo/bin:${PATH}"
set -a
# shellcheck disable=SC1091
source "${HOME}/.arb-swqos.env"
set +a
code="$(curl -sS -o /tmp/swqos_acc.json -w '%{http_code}' \
  -H "Authorization: Bearer ${SWQOS_KEY}" \
  https://send.swqos.com/v1/account || true)"
echo "HTTPS:${code}"
python3 - <<'PY'
import json
from pathlib import Path
p = Path("/tmp/swqos_acc.json")
if not p.exists() or p.stat().st_size == 0:
    print("empty body")
    raise SystemExit(0)
d = json.loads(p.read_text())
if isinstance(d, dict):
    print("enabled", d.get("enabled"), "bal", d.get("balance_lamports"), "keys", sorted(d.keys()))
else:
    print("body_type", type(d).__name__)
PY
rm -f /tmp/swqos_acc.json
cd /home/louis/arb-exec
cmake --build build --target exec_swqos
./build/exec_swqos
echo "EXIT:$?"
