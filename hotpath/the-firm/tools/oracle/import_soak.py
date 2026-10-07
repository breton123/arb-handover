#!/usr/bin/env python3
"""PUMP-ORACLE-002 importer. Offline. No RPC.

Reads state008/mismatch/*.json and optional n015_tx/*.json.
Writes one jsonl row per usable Pump case for C admission.

Certified prestate is only what AUTH already had. Recovered V is a
tag (invert hit), never injected as pre_complete=1.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

SELL = bytes.fromhex("33e685a4017f83ad")
BUY_EQ = bytes.fromhex("c62e1552b4d9e870")
BUY_OUT = bytes.fromhex("66063d1201daebea")
PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
T22 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"

DEFAULT_ROOTS = (
    os.environ.get("FIRM_SOAK_ROOT", ""),
    "/data/bsc/captures/soak_fastsoak_20260925/state008",
    str(Path.home() / "captures/state008"),
)


def _dir_amt(b: dict) -> tuple[int | None, int | None]:
    n = b.get("n") or {}
    d = b.get("direction")
    if d is None:
        d = n.get("direction")
    a = b.get("amount_in")
    if a is None:
        a = n.get("amount_in")
    if d is None or a is None:
        return None, None
    return int(d), int(a)


def _vq(sb: dict) -> int | None:
    if "virtual_quote_reserves" in sb:
        return int(sb["virtual_quote_reserves"])
    if "virtual_quote" in sb:
        return int(sb["virtual_quote"])
    return None


def _vq_field_present(sb: dict) -> bool:
    return "virtual_quote_reserves" in sb or "virtual_quote" in sb


def _fees_present(sb: dict) -> bool:
    return "lp_fee_bps" in sb or "protocol_fee_bps" in sb or "creator_fee_bps" in sb


def _ix_from_dir(d: int, sb: dict, pu: dict, ain: int | None) -> int:
    if d == 1:
        return 1
    if (
        ain is not None
        and "reserve_base" in sb
        and "reserve_base" in pu
        and int(pu["reserve_base"]) == int(sb["reserve_base"]) - int(ain)
    ):
        return 3
    return 2


def classify_tag(b: dict) -> tuple[str, int, int]:
    """tag, ix, pre_complete. Recovered V is never a completeness source."""
    bucket = b.get("bucket") or ""
    d, ain = _dir_amt(b)
    sb = b.get("s_before") or {}
    pu = b.get("published_s") or {}
    ix = _ix_from_dir(int(d or 0), sb, pu, ain)
    if bucket == "wrong_transaction_association":
        return "assoc", 4, 0
    if bucket == "unknown_disc":
        return "unknown_disc", 5, 1
    if d == 1 and ain is not None and "reserve_base" in sb and "reserve_base" in pu:
        if int(pu["reserve_base"]) != int(sb["reserve_base"]) + int(ain):
            return "wrong_n", 1, 0
    if not _fees_present(sb):
        return "missing_fee", ix, 0
    if not _vq_field_present(sb):
        return "missing_virtual", ix, 0
    return "auth_v", ix, 1


def _prog_of(ix: dict, keys: list[str]) -> str:
    prog = ix.get("programId")
    if prog is None and isinstance(ix.get("programIdIndex"), int):
        pi = ix["programIdIndex"]
        prog = keys[pi] if pi < len(keys) else ""
    return str(prog or "")


def tx_cpis(tx: dict | None) -> tuple[int, int, int, int]:
    """n_cpi, ix_override (0 keep), wsol, token2022."""
    if not tx:
        return 1, 0, 0, 0
    n = 0
    wsol = 0
    t22 = 0
    keys = []
    msg = (tx.get("transaction") or {}).get("message") or {}
    accs = msg.get("accountKeys") or []
    for k in accs:
        if isinstance(k, str):
            keys.append(k)
        elif isinstance(k, dict):
            keys.append(k.get("pubkey") or "")
        if T22 in str(k):
            t22 = 1
    inners = {}
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        inners.setdefault(int(g.get("index") or 0), []).extend(g.get("instructions") or [])
    outers = msg.get("instructions") or []
    for i, ox in enumerate(outers):
        prog = _prog_of(ox, keys)
        if prog == PUMP:
            n += 1
        if prog == T22:
            t22 = 1
        if "11111111111111111111111111111111" in prog:
            wsol = 1
        for ix in inners.get(i) or []:
            p2 = _prog_of(ix, keys)
            if p2 == PUMP:
                n += 1
            if p2 == T22:
                t22 = 1
            if p2.endswith("11111111111111111111111111111111") or "Tokenkeg" in p2:
                wsol = 1
    if n == 0:
        n = 1
    return n, 0, wsol, t22


def load_one_tx(txdir: Path | None, hx: str) -> dict | None:
    if txdir is None or not hx:
        return None
    p = txdir / f"{hx}.json"
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def row_from_mismatch(b: dict, tx: dict | None) -> dict | None:
    if b.get("kind") != "pump":
        return None
    sb = b.get("s_before") or {}
    pu = b.get("published_s") or {}
    if "reserve_base" not in sb or "reserve_base" not in pu:
        return None
    d, ain = _dir_amt(b)
    if d is None or ain is None:
        return None
    tag, ix, complete = classify_tag(b)
    n_cpi, ix_ov, wsol, t22 = tx_cpis(tx)
    if ix_ov:
        ix = ix_ov
    if b.get("declared_ix") is not None:
        ix = int(b["declared_ix"])
    if b.get("n_cpi") is not None:
        n_cpi = int(b["n_cpi"])
    if b.get("wsol"):
        wsol = 1
    if b.get("t22"):
        t22 = 1
    hx = b.get("trigger_sig") or b.get("sig") or ""
    if not hx:
        for w in b.get("staged_writes") or []:
            if w.get("txn_sig"):
                hx = w["txn_sig"]
                break
    vq = _vq(sb)
    if vq is None:
        vq = 0
    return {
        "id": (hx[:16] if isinstance(hx, str) else "row") or "row",
        "sig": hx if isinstance(hx, str) else "",
        "slot": int(b.get("slot") or 0),
        "dir": int(d),
        "ain": int(ain),
        "rb": int(sb["reserve_base"]),
        "rq": int(sb["reserve_quote"]),
        "vq": int(vq),
        "lp": int(sb.get("lp_fee_bps") or 0),
        "proto": int(sb.get("protocol_fee_bps") or 0),
        "cr": int(sb.get("creator_fee_bps") or 0),
        "prb": int(pu["reserve_base"]),
        "prq": int(pu["reserve_quote"]),
        "pre_complete": complete,
        "ix": ix,
        "n_cpi": n_cpi,
        "wsol": wsol,
        "t22": t22,
        "tag": tag,
        "bucket": b.get("bucket") or "",
    }


def iter_mismatch(root: Path):
    mis = root / "mismatch"
    if not mis.is_dir():
        return
    for fp in mis.glob("*.json"):
        try:
            yield json.loads(fp.read_text(encoding="utf-8"))
        except Exception:
            continue


def iter_scoreboard(path: Path):
    if not path.is_file():
        return
    d = json.loads(path.read_text(encoding="utf-8"))
    for rec in (d.get("examples") or {}).values():
        if isinstance(rec, dict):
            yield rec


def iter_seed(seed: Path):
    if seed.is_file() and seed.suffix == ".json":
        try:
            yield json.loads(seed.read_text(encoding="utf-8"))
        except Exception:
            return
        return
    if not seed.is_dir():
        return
    for fp in seed.glob("*.json"):
        try:
            yield json.loads(fp.read_text(encoding="utf-8"))
        except Exception:
            continue


def find_root(explicit: str | None) -> Path | None:
    cands = []
    if explicit:
        cands.append(explicit)
    cands.extend(DEFAULT_ROOTS)
    for c in cands:
        if not c:
            continue
        p = Path(c)
        if (p / "mismatch").is_dir():
            return p
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--txdir", default=None)
    ap.add_argument("--scoreboard", default=None)
    ap.add_argument("--seed", default="tests/fixtures/oracle/soak")
    ap.add_argument("--no-seed", action="store_true")
    ap.add_argument("--out", default="tests/fixtures/oracle/pump-oracle-002.jsonl")
    args = ap.parse_args()

    rows = []
    seen = set()
    root = find_root(args.root)
    if root is not None:
        txdir = Path(args.txdir) if args.txdir else root.parent / "n015_tx"
        if not txdir.is_dir():
            txdir = root / "n015_tx"
        if not txdir.is_dir():
            txdir = None
        for b in iter_mismatch(root):
            hx = b.get("trigger_sig") or b.get("sig") or ""
            tx = load_one_tx(txdir, hx) if hx else None
            r = row_from_mismatch(b, tx)
            if r and r["sig"] not in seen:
                seen.add(r["sig"])
                rows.append(r)

    sb = Path(args.scoreboard) if args.scoreboard else Path(
        "../TheMoneyMaker/arb-cap/state008/SCOREBOARD.json"
    )
    if not sb.is_file():
        sb = Path("C:/Users/louis/Desktop/TheMoneyMaker/arb-cap/state008/SCOREBOARD.json")
    for b in iter_scoreboard(sb):
        r = row_from_mismatch(b, None)
        if r and r["sig"] not in seen:
            seen.add(r["sig"])
            rows.append(r)

    if not args.no_seed:
        for b in iter_seed(Path(args.seed)):
            r = row_from_mismatch(b, None)
            if r and r["sig"] not in seen:
                seen.add(r["sig"])
                rows.append(r)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, separators=(",", ":")) + "\n")
    print(f"wrote {len(rows)} rows -> {out}  soak_root={root}")
    return 0 if rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
