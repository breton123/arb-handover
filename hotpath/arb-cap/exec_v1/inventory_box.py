#!/usr/bin/env python3
"""Read-only Frankfurt inventory. Writes P0_INVENTORY.md.

Missing paths are unverified. Does not arm, deploy, or send.
Optional public RPC getAccountInfo for OUR_EXEC. No API key is read.
"""
from __future__ import annotations

import base64
import json
import os
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

OUR_EXEC = "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K"
UPGRADEABLE = "BPFLoaderUpgradeab1e11111111111111111111111"
WALLET = "HHNzjTABPAbPY5uEXU7NTvfmcD9RYTdXtLntyKAtpXLX"
RPC = os.environ.get("SOLANA_RPC_URL") or "https://api.mainnet-beta.solana.com"

HOME = Path("/home/louis")
PATHS = {
    "audit": HOME / "captures/paper_orbit/opp_synced.jsonl",
    "univ": HOME / "captures/paper_orbit/liveuniv.json",
    "stats": HOME / "captures/paper_orbit/stats.json",
    "alt_plane": HOME / "arb-exec/.deploy/alt_plane.json",
    "alt_report": HOME / "arb-cap/oneshot/ALT_PLANE.json",
    "hops_plane": HOME / "arb-cap/fam6/hops_plane.json",
    "hops_sim": HOME / "arb-cap/fam6/SIM.json",
    "hops_size": HOME / "arb-cap/fam6/SIZE.json",
    "ready_008_arm": HOME / "captures/state008/READY",
    "ready_008_live": Path("/data/bsc/captures/soak_pump016_20260926/state008/READY"),
    "ready_007": HOME / "captures/state007/READY",
    "armed": HOME / "arb-cap/oneshot/ARMED",
    "disarmed": HOME / "arb-cap/oneshot/DISARMED",
    "result": HOME / "arb-cap/oneshot/RESULT.json",
    "cooldown": HOME / "arb-cap/oneshot/COOLDOWN.json",
    "racer_sock": HOME / "arb-cap/oneshot/swqos.sock",
}


def file_row(path: Path) -> str:
    if not path.exists():
        return f"- `{path.as_posix()}` — missing on this machine. **unverified**"
    st = path.stat()
    age = datetime.now(timezone.utc).timestamp() - st.st_mtime
    return (
        f"- `{path.as_posix()}` — {st.st_size} bytes, mtime age {int(age)}s"
    )


def count_kinds(path: Path, limit_bytes: int = 80_000_000) -> str:
    if not path.exists():
        return "audit kinds: **unverified** (file missing)"
    kinds: dict[str, int] = {}
    gross_pos = cap_hurdle = mut = would = 0
    tx_exact = journal_rr = 0
    n = 0
    size = path.stat().st_size
    with path.open("rb") as handle:
        if size > limit_bytes:
            handle.seek(size - limit_bytes)
            handle.readline()
        for raw in handle:
            if b'"kind":' not in raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError:
                continue
            kind = str(rec.get("kind") or "")
            kinds[kind] = kinds.get(kind, 0) + 1
            n += 1
            if kind == "gate":
                if rec.get("gross_pos"):
                    gross_pos += 1
                if rec.get("cap_hurdle"):
                    cap_hurdle += 1
                if rec.get("mut_authoritative"):
                    mut += 1
                if rec.get("would_send_new"):
                    would += 1
            elif kind == "opp_synced":
                if rec.get("tx_exact") == 1:
                    tx_exact += 1
                if rec.get("race_ready") == 1:
                    journal_rr += 1
    window = "tail 80MB" if size > limit_bytes else "full file"
    return (
        f"audit rows ({window}) n={n} kinds={kinds} "
        f"gross_pos={gross_pos} cap_hurdle={cap_hurdle} "
        f"mut_authoritative={mut} would_send_new={would} "
        f"opp tx_exact={tx_exact} journal_race_ready={journal_rr}"
    )


def plane_counts(path: Path) -> str:
    if not path.exists():
        return f"`{path.name}` — **unverified**"
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return f"`{path.name}` — unreadable"
    routes = doc.get("routes") or []
    upper = sum(1 for r in routes if r.get("RACE_READY") and r.get("tmpl0") and r.get("tmpl1"))
    lower = sum(1 for r in routes if r.get("race_ready"))
    vector = sum(1 for r in routes if r.get("vector_ready"))
    over = sum(1 for r in routes if int(r.get("raw") or 0) > 1232)
    return (
        f"`{path.name}` routes={len(routes)} "
        f"oneshot_RACE_READY_tmpl={upper} hops_race_ready={lower} "
        f"vector_ready={vector} raw_gt_1232={over}"
    )


def rpc_account(pubkey: str) -> dict:
    body = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "getAccountInfo",
        "params": [pubkey, {"encoding": "base64"}],
    }).encode("utf-8")
    req = urllib.request.Request(
        RPC, data=body, headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    value = ((payload.get("result") or {}).get("value"))
    if not value:
        return {"exists": False}
    data = value.get("data")
    space = None
    if isinstance(data, list) and data and isinstance(data[0], str):
        try:
            space = len(base64.b64decode(data[0]))
        except Exception:
            space = None
    return {
        "exists": True,
        "owner": value.get("owner"),
        "executable": bool(value.get("executable")),
        "lamports": value.get("lamports"),
        "space": space,
    }


def rpc_section() -> str:
    lines = [f"RPC `{RPC}` (public, no key from .env)."]
    try:
        info = rpc_account(OUR_EXEC)
    except Exception as exc:
        lines.append(f"- OUR_EXEC `{OUR_EXEC}` — **unverified** ({type(exc).__name__})")
        info = None
    if info is not None:
        if not info.get("exists"):
            lines.append(f"- OUR_EXEC `{OUR_EXEC}` — account absent")
        else:
            owner_ok = info.get("owner") == UPGRADEABLE
            lines.append(
                f"- OUR_EXEC `{OUR_EXEC}` — exists executable={info.get('executable')} "
                f"owner_upgradeable={owner_ok} lamports={info.get('lamports')} "
                f"space={info.get('space')}"
            )
    try:
        bal = rpc_account(WALLET)
    except Exception as exc:
        lines.append(f"- wallet `{WALLET}` — **unverified** ({type(exc).__name__})")
        bal = None
    if bal is not None:
        if not bal.get("exists"):
            lines.append(f"- wallet `{WALLET}` — account absent")
        else:
            lines.append(
                f"- wallet `{WALLET}` — lamports={bal.get('lamports')} "
                f"(native only; WSOL not queried)"
            )
    return "\n".join(lines)


def main() -> int:
    here = Path(__file__).resolve().parent
    out = here / "P0_INVENTORY.md"
    ssh_note = os.environ.get("EXEC_V1_SSH") or (
        "SSH to louis@195.242.152.178 was not available from this checkout "
        "(publickey denied; documented identity file absent). "
        "Live process list is **unverified**."
    )
    parts = [
        "# P0 inventory",
        "",
        f"Written {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')} "
        "from this machine. Counts below are from files that exist here. "
        "Anything else is unverified. No send was attempted.",
        "",
        "## Box",
        "",
        ssh_note,
        "",
        "Processes `paper_orbit`, `state008.py`, `exec_swqos_racer`, `feed_live`: **unverified**.",
        "",
        "## Files",
        "",
    ]
    for path in PATHS.values():
        parts.append(file_row(path))
    parts += [
        "",
        "## Audit",
        "",
        count_kinds(PATHS["audit"]),
        "",
        "Gate field for a positive cap quote is `cap_pos`. "
        "`opp_synced` uses `send_quote.cap_ok`.",
        "",
        "## Planes",
        "",
        "Oneshot loads uppercase `RACE_READY` plus `tmpl0`/`tmpl1`. "
        "hops_live writes lowercase `race_ready` and does not publish those templates. "
        "The two are not combined.",
        "",
        plane_counts(PATHS["alt_plane"]),
        plane_counts(PATHS["alt_report"]),
        plane_counts(PATHS["hops_plane"]),
        "",
        "## AUTH ready",
        "",
        "Oneshot arms only `/home/louis/captures/state008/READY`. "
        "The live writer is `STATE008_CAP` "
        "(`/data/bsc/captures/soak_pump016_20260926/state008`). "
        "Funnel/audit default to that live cap. `state007/READY` does not arm.",
        "",
        "## Chain",
        "",
        rpc_section(),
        "",
        "## Not run",
        "",
        "`hops_live.py` deploy, sim7, and publish were not run. "
        "sim7 wraps 50_000_000 lamports and creates ATAs. publish creates ATAs. "
        "deploy upgrades a program. Those stay off without an explicit instruction.",
        "",
    ]
    out.write_text("\n".join(parts) + "\n", encoding="utf-8")
    print(f"wrote {out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
