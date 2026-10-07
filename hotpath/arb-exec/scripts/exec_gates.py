"""Exec-side send gates. No RPC, no socket, no FUNDED.

Two different race_ready flags. They are never OR'd:

- Journal ``race_ready`` on an opp_synced row is ``tx_exact`` from paper_orbit.
- Plane ``RACE_READY`` means the alt plane compiled both directions and the
  ATAs existed when that file was written. It is not a Custom(6) proof.
  hops_live's lowercase ``race_ready`` is a third field and does not satisfy
  this gate. A pubkey shared by two DLMM/Pump pairs is not sendable.

The send floor is the oneshot fee schedule, 525_000 lamports. Paper's
per-sequence hops_hurdle_for_seq is a search predicate and is not consulted
here. AUTH coherence is the STATE-008 READY file. The state007 path is a
retired alias and does not open the gate.
"""
from __future__ import annotations

import os
from pathlib import Path

MAX_IN = 50_000_000
MIN_IN = 10_000_000
# Family 6 is dlmm-dlmm. The oneshot template is DLMM↔Pump only.
ROUTE0_FAMILIES = frozenset({0, 5})
SUPPORTED_SEQ = frozenset({"dlmm-pump", "pump-dlmm"})
CU_LIMIT = 400_000
CU_PRICE = 800_000
SIG_FEE = 5_000
SWQOS_UNIT = 150_000
SAFETY = 50_000


def wire_hurdle(cu_limit: int = CU_LIMIT, cu_price: int = CU_PRICE) -> int:
    """Fee the wire actually bills: signature + requested CU limit * price + SWQOS + safety.

    Consumed CU is not the bill. Solana charges the requested limit.
    """
    onchain = int(cu_limit) * int(cu_price) // 1_000_000 + SIG_FEE
    return onchain + SWQOS_UNIT + SAFETY


HURDLE = wire_hurdle()

AUTH_READY_DEFAULT = "/home/louis/captures/state008/READY"
STATE007_READY_ALIAS = "/home/louis/captures/state007/READY"
# Live STATE-008 writer. Audit and funnel read this. Oneshot still arms only
# on AUTH_READY_DEFAULT (state007 and arbitrary AUTH_READY overrides stay closed).
CURRENT_AUTH_CAP_DEFAULT = "/data/bsc/captures/soak_pump016_20260926/state008"


def current_auth_cap(env: dict | None = None) -> tuple[str, str | None]:
    """Directory the live state008.py writes (STATE008_CAP or the Pump016 soak)."""
    src = env if env is not None else os.environ
    cap = (src.get("STATE008_CAP") or CURRENT_AUTH_CAP_DEFAULT).replace("\\", "/").rstrip("/")
    if cap == str(Path(STATE007_READY_ALIAS).parent) or "/captures/state007" in cap:
        return cap, "state007_ready_cannot_arm"
    return cap, None


def current_auth_ready(env: dict | None = None) -> Path:
    cap, _err = current_auth_cap(env)
    return Path(cap) / "READY"

_B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58encode(raw: bytes) -> str:
    n = int.from_bytes(raw, "big")
    chars: list[str] = []
    while n:
        n, rem = divmod(n, 58)
        chars.append(_B58[rem])
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    body = "".join(reversed(chars)) if chars else ""
    return ("1" * pad) + body


def pool_pubkey(pool: str) -> str:
    """Journal pool is 32-byte hex. Plane keys are base58. Pass base58 through."""
    text = str(pool or "").strip()
    if len(text) == 64:
        try:
            raw = bytes.fromhex(text)
        except ValueError:
            return text
        if len(raw) == 32:
            return b58encode(raw)
    return text


def v1_executable(family, n_hop, seq: str | None = None) -> bool:
    """DLMM↔Pump families 0 and 5, under 3 hops. Family 6 and 3-hop are not oneshot shapes."""
    if seq:
        return seq in SUPPORTED_SEQ
    try:
        fam = int(family)
    except (TypeError, ValueError):
        return False
    try:
        hops = int(n_hop) if n_hop is not None else 0
    except (TypeError, ValueError):
        hops = 0
    if hops >= 3:
        return False
    return fam in ROUTE0_FAMILIES


def resolve_auth_ready(env_value: str | None) -> tuple[str, str | None]:
    """The only READY file that may arm is the STATE-008 path. state007 never arms."""
    if not env_value:
        return AUTH_READY_DEFAULT, None
    norm = env_value.replace("\\", "/").rstrip("/")
    if norm == STATE007_READY_ALIAS or "/captures/state007/" in norm:
        return norm, "state007_ready_cannot_arm"
    if norm != AUTH_READY_DEFAULT:
        return norm, "auth_ready_path_not_mandatory"
    return norm, None


def ambiguous_pool_keys(routes: list[dict] | None) -> set[str]:
    """Pubkeys that sit on two different DLMM/Pump pairs. Last-write-wins would send the wrong template."""
    owners: dict[str, tuple] = {}
    ambiguous: set[str] = set()
    for r in routes or []:
        pair = (r.get("dlmm"), r.get("pump"))
        for key in pair:
            if not key:
                continue
            key = str(key)
            prev = owners.get(key)
            if prev is not None and prev != pair:
                ambiguous.add(key)
            owners[key] = pair
    return ambiguous


def index_plane(routes: list[dict] | None) -> dict[str, dict]:
    """Index alt-plane rows. A pool is plane-ready only with uppercase RACE_READY and both templates.

    A pubkey shared by two pairs is not plane-ready. The send must not pick one pair by file order.
    """
    ambiguous = ambiguous_pool_keys(routes)
    by: dict[str, dict] = {}
    for r in routes or []:
        tmpl0 = r.get("tmpl0")
        tmpl1 = r.get("tmpl1")
        plane_ready = bool(r.get("RACE_READY")) and bool(tmpl0) and bool(tmpl1)
        rec = {
            "dlmm": r.get("dlmm"),
            "pump": r.get("pump"),
            "plane_race_ready": 1 if plane_ready else 0,
            "hops_race_ready": int(r.get("race_ready") or 0),
            "vector_ready": int(r.get("vector_ready") or 0),
            "tmpl0": tmpl0,
            "tmpl1": tmpl1,
            "ambiguous": 0,
        }
        for key in (r.get("dlmm"), r.get("pump")):
            if not key:
                continue
            row = dict(rec)
            if str(key) in ambiguous:
                row["plane_race_ready"] = 0
                row["ambiguous"] = 1
            by[str(key)] = row
    return by


def plane_row(plane: dict[str, dict], pool: str) -> dict | None:
    if not pool:
        return None
    row = plane.get(pool)
    if row is not None:
        return row
    return plane.get(pool_pubkey(pool))


def race_flags(journal_race_ready, plane_race_ready) -> dict:
    """Side-by-side flags. ``both`` is an AND. There is no OR."""
    tx_exact = 1 if journal_race_ready == 1 or journal_race_ready is True else 0
    plane = 1 if plane_race_ready == 1 or plane_race_ready is True else 0
    return {
        "tx_exact": tx_exact,
        "plane_race_ready": plane,
        "both": 1 if tx_exact and plane else 0,
    }


def route_blocked(pool: str, cool: dict | None) -> bool:
    routes = (cool or {}).get("routes") or {}
    if (routes.get(pool) or {}).get("SEND_BLOCKED"):
        return True
    b58 = pool_pubkey(pool)
    if b58 != pool and (routes.get(b58) or {}).get("SEND_BLOCKED"):
        return True
    pref = (b58 or pool)[:8]
    for key, row in routes.items():
        if row.get("SEND_BLOCKED") and (
            str(key).startswith(pref) or (b58 or pool).startswith(str(key))
        ):
            return True
    return False


def framed_ready(rec: dict, plane: dict[str, dict], ready_exists: bool) -> bool:
    """FRAMED and journal tx_exact and STATE-008 READY and plane RACE_READY.

    ``pool in plane`` is not enough. The row's plane_race_ready flag must be 1.
    Journal race_ready == 1 does not imply the plane flag.
    """
    fr = rec.get("frame") or {}
    pool = str(rec.get("pool") or "")
    row = plane_row(plane, pool)
    flags = race_flags(
        rec.get("race_ready"),
        (row or {}).get("plane_race_ready"),
    )
    return (
        fr.get("class") == "framed"
        and flags["both"] == 1
        and ready_exists
        and bool(pool)
        and row is not None
        and bool(row.get("tmpl0"))
        and bool(row.get("tmpl1"))
    )


def size_gate(rec: dict, plane: dict[str, dict], ready_exists: bool,
              cool: dict | None = None) -> tuple[int, int] | None:
    """Frozen 50_000_000 send. Gross must clear the oneshot hurdle, not the paper hurdle."""
    arb = rec.get("arb") or {}
    if not framed_ready(rec, plane, ready_exists):
        return None
    direction = arb.get("direction")
    if direction is None or int(direction) not in (0, 1):
        return None
    if route_blocked(str(rec.get("pool") or ""), cool):
        return None
    sq = rec.get("send_quote") or {}
    if not sq.get("cap_ok"):
        return None
    gross = int(sq.get("cap_gross") or 0)
    if MAX_IN < MIN_IN or gross <= HURDLE:
        return None
    arb_seq = rec.get("seq") or arb.get("seq")
    arb_fam = rec.get("family")
    if arb_fam is None:
        arb_fam = arb.get("family")
    arb_hops = rec.get("n_hop")
    if arb_hops is None:
        arb_hops = arb.get("n_hop")
    if arb_seq or arb_fam is not None or arb_hops is not None:
        if not v1_executable(arb_fam, arb_hops, str(arb_seq) if arb_seq else None):
            return None
    return MAX_IN, gross


def classify_opp(rec: dict, plane: dict[str, dict], ready_exists: bool,
                 cool: dict | None = None, family=None, n_hop=None,
                 seq: str | None = None) -> str:
    """One drop bucket for an opp_synced row. Same order as size_gate."""
    if family is not None or n_hop is not None or seq:
        if not v1_executable(family, n_hop, seq):
            return "not_v1_executable"
    fr = rec.get("frame") or {}
    if fr.get("class") != "framed":
        return "not_framed"
    if rec.get("race_ready") != 1:
        return "ix_only"
    if not ready_exists:
        return "ready_file_missing"
    pool = str(rec.get("pool") or "")
    row = plane_row(plane, pool)
    if row is None:
        return "plane_miss"
    if int(row.get("ambiguous") or 0) == 1:
        return "ambiguous_pool"
    if int(row.get("plane_race_ready") or 0) != 1 or not row.get("tmpl0") or not row.get("tmpl1"):
        return "plane_not_race_ready"
    arb = rec.get("arb") or {}
    direction = arb.get("direction")
    if direction is None or int(direction) not in (0, 1):
        return "bad_direction"
    if route_blocked(pool, cool):
        return "cooldown"
    sq = rec.get("send_quote") or {}
    if not sq.get("cap_ok"):
        return "cap_not_ok"
    gross = int(sq.get("cap_gross") or 0)
    if MAX_IN < MIN_IN or gross <= HURDLE:
        return "below_oneshot_hurdle"
    if size_gate(rec, plane, ready_exists, cool) is None:
        return "size_gate"
    return "would_fire"
