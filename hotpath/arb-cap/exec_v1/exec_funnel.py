#!/usr/bin/env python3
"""Read-only exec funnel. One drop bucket per would_send_new and opp_synced row.

Does not arm, open the racer, or call SWQOS. Historical replay reads the whole
audit file (not EOF-only).

Journal race_ready is tx_exact. Plane RACE_READY is Custom(6) plus templates.
The summary prints them side by side and never ORs them.
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-exec" / "scripts"))
import exec_gates as gates  # noqa: E402

DEFAULT_AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
DEFAULT_UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
DEFAULT_PLANE = Path("/home/louis/arb-exec/.deploy/alt_plane.json")
DEFAULT_PLANE_REPORT = Path("/home/louis/arb-cap/oneshot/ALT_PLANE.json")
DEFAULT_READY = gates.current_auth_ready()
DEFAULT_COOL = Path("/home/louis/arb-cap/oneshot/COOLDOWN.json")


def load_json(path: Path, default):
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def iter_audit(path: Path):
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if '"kind":' not in line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(rec, dict) and rec.get("kind"):
                yield rec


def univ_index(doc: dict) -> dict[int, str]:
    out: dict[int, str] = {}
    pools = doc.get("pools") or []
    for i, pool in enumerate(pools):
        pk = pool.get("pubkey") or pool.get("pk") or ""
        idx = pool.get("idx", i)
        try:
            idx = int(idx)
        except (TypeError, ValueError):
            continue
        if pk:
            out[idx] = str(pk)
    return out


def load_plane(plane_path: Path, report_path: Path) -> dict[str, dict]:
    routes = []
    for path in (plane_path, report_path):
        doc = load_json(path, {})
        routes.extend(doc.get("routes") or [])
    return gates.index_plane(routes)


def in_window(rec: dict, now: float | None, seconds: float | None) -> bool:
    if now is None or seconds is None:
        return True
    ts = rec.get("ts")
    # opp_synced rows have no unix ts. A window that cannot see a timestamp
    # does not include them. The full-file run (no --seconds) still does.
    if not isinstance(ts, (int, float)):
        return False
    if ts > 1e12:
        ts = ts / 1e9
    return (now - ts) <= seconds


def opp_key(rec: dict) -> tuple:
    n = rec.get("n") or {}
    fresh = rec.get("fresh") or {}
    return (n.get("pool_idx"), fresh.get("shred_slot"))


def index_opps(opps: list[dict]) -> dict[tuple, list[dict]]:
    by: dict[tuple, list[dict]] = {}
    for rec in opps:
        by.setdefault(opp_key(rec), []).append(rec)
    return by


def pick_opp(cands: list[dict]) -> dict | None:
    if not cands:
        return None
    for rec in cands:
        if rec.get("race_ready") == 1 or rec.get("tx_exact") == 1:
            return rec
    return cands[0]


def classify_gate(gate: dict, opp: dict | None, plane, ready: bool, cool, univ) -> str:
    if not gates.v1_executable(gate.get("family"), gate.get("n_hop"), gate.get("seq")):
        return "not_v1_executable"
    if opp is None:
        return "no_opp_synced"
    pool = str(opp.get("pool") or "")
    if not pool:
        idx = gate.get("pool_idx")
        try:
            pool = univ.get(int(idx), "")
        except (TypeError, ValueError):
            pool = ""
        if pool:
            opp = dict(opp)
            opp["pool"] = pool
    return gates.classify_opp(
        opp, plane, ready, cool,
        family=gate.get("family"), n_hop=gate.get("n_hop"), seq=gate.get("seq"),
    )


def row_flags(rec: dict, plane) -> dict:
    pool = str(rec.get("pool") or "")
    prow = gates.plane_row(plane, pool) or {}
    journal = rec.get("race_ready")
    if journal is None and rec.get("tx_exact") is not None:
        journal = rec.get("tx_exact")
    return gates.race_flags(journal, prow.get("plane_race_ready"))


def reduce(audit: Path, univ_path: Path, plane_path: Path, report_path: Path,
           ready_path: Path, cool_path: Path, seconds: float | None,
           now: float | None) -> tuple[list[dict], Counter]:
    univ = univ_index(load_json(univ_path, {}))
    plane = load_plane(plane_path, report_path)
    cool = load_json(cool_path, {"routes": {}})
    ready = ready_path.exists()
    gates_rows = []
    opps = []
    for rec in iter_audit(audit):
        if not in_window(rec, now, seconds):
            continue
        kind = rec.get("kind")
        if kind == "gate":
            gates_rows.append(rec)
        elif kind == "opp_synced":
            opps.append(rec)
    by_opp = index_opps(opps)
    out = []
    counts: Counter = Counter()
    for gate in gates_rows:
        if not gate.get("would_send_new"):
            continue
        key = (gate.get("pool_idx"), gate.get("shred_slot"))
        opp = pick_opp(by_opp.get(key) or [])
        bucket = classify_gate(gate, opp, plane, ready, cool, univ)
        flags = row_flags(opp or {}, plane) if opp else {
            "tx_exact": 0, "plane_race_ready": 0, "both": 0,
        }
        item = {
            "source": "would_send_new",
            "bucket": bucket,
            "pool_idx": gate.get("pool_idx"),
            "family": gate.get("family"),
            "n_hop": gate.get("n_hop"),
            "seq": gate.get("seq"),
            "exec_fam": gate.get("exec_fam"),
            "cap_hurdle": gate.get("cap_hurdle"),
            "cap_gross": gate.get("cap_gross"),
            "paper_hurdle": "hops_hurdle_for_seq",
            "send_hurdle": gates.HURDLE,
            "shred_slot": gate.get("shred_slot"),
            "tx_exact": flags["tx_exact"],
            "plane_race_ready": flags["plane_race_ready"],
            "race_both": flags["both"],
        }
        out.append(item)
        counts[bucket] += 1
    for opp in opps:
        n = opp.get("n") or {}
        bucket = gates.classify_opp(opp, plane, ready, cool)
        flags = row_flags(opp, plane)
        sq = opp.get("send_quote") or {}
        item = {
            "source": "opp_synced",
            "bucket": bucket,
            "pool": opp.get("pool"),
            "pool_b58": gates.pool_pubkey(str(opp.get("pool") or "")),
            "pool_idx": n.get("pool_idx"),
            "cap_ok": sq.get("cap_ok"),
            "cap_gross": sq.get("cap_gross"),
            "send_hurdle": gates.HURDLE,
            "tx_exact": flags["tx_exact"],
            "plane_race_ready": flags["plane_race_ready"],
            "race_both": flags["both"],
        }
        out.append(item)
        counts[("opp", bucket)] += 1
    return out, counts


def summary_text(rows: list[dict], counts: Counter, ready: bool) -> str:
    wsn = [r for r in rows if r["source"] == "would_send_new"]
    opp = [r for r in rows if r["source"] == "opp_synced"]
    lines = [
        f"would_send_new={len(wsn)} opp_synced={len(opp)} auth_ready={int(ready)}",
        f"send_hurdle={gates.HURDLE} (oneshot fee schedule; paper cap_hurdle is not the send floor)",
        "would_send_new buckets:",
    ]
    buckets = (
        "not_v1_executable", "no_opp_synced", "ix_only", "plane_miss",
        "plane_not_race_ready", "ready_file_missing", "cooldown",
        "bad_direction", "cap_not_ok", "below_oneshot_hurdle", "would_fire",
        "not_framed", "size_gate",
    )
    for name in buckets:
        n = sum(1 for r in wsn if r["bucket"] == name)
        if n:
            lines.append(f"  {name}={n}")
    both = sum(1 for r in opp if r["race_both"])
    tx = sum(1 for r in opp if r["tx_exact"])
    plane = sum(1 for r in opp if r["plane_race_ready"])
    lines.append(
        f"opp_synced flags tx_exact={tx} plane_race_ready={plane} "
        f"both_and={both} (not OR)"
    )
    lines.append("opp_synced buckets:")
    for name in buckets:
        n = sum(1 for r in opp if r["bucket"] == name)
        if n:
            lines.append(f"  {name}={n}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="read-only exec funnel")
    ap.add_argument("--audit", type=Path, default=DEFAULT_AUDIT)
    ap.add_argument("--univ", type=Path, default=DEFAULT_UNIV)
    ap.add_argument("--plane", type=Path, default=DEFAULT_PLANE)
    ap.add_argument("--plane-report", type=Path, default=DEFAULT_PLANE_REPORT)
    ap.add_argument("--ready", type=Path, default=None)
    ap.add_argument(
        "--auth-cap", type=Path, default=None,
        help="STATE-008 cap dir (READY lives here). Default: STATE008_CAP or Pump016 soak.",
    )
    ap.add_argument("--cooldown", type=Path, default=DEFAULT_COOL)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--seconds", type=float, default=None)
    args = ap.parse_args(argv)
    if args.ready is None:
        if args.auth_cap is not None:
            args.ready = Path(args.auth_cap) / "READY"
        else:
            args.ready = gates.current_auth_ready()
    if not args.audit.exists():
        print(f"UNVERIFIED audit missing {args.audit}", flush=True)
        return 2
    import time
    now = time.time() if args.seconds else None
    rows, counts = reduce(
        args.audit, args.univ, args.plane, args.plane_report,
        args.ready, args.cooldown, args.seconds, now,
    )
    text = summary_text(rows, counts, args.ready.exists())
    print(f"auth_ready_path={args.ready} exists={int(args.ready.exists())}", flush=True)
    print(text, flush=True)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("w", encoding="utf-8") as handle:
            for row in rows:
                handle.write(json.dumps(row, separators=(",", ":")) + "\n")
        print(f"wrote {args.out} rows={len(rows)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
