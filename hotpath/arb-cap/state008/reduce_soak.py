#!/usr/bin/env python3
"""Offline reducer for the frozen FASTSOAK. Does not change live state logic.

Reads SHADOW.jsonl + mismatch/*.json and clusters by venue + field/signature shape.
Pump 6% exact is the first question: one dominant class or many.
"""
from __future__ import annotations

import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

PUMP_CLASSES = (
    "reserve_mismatch",
    "vault_mismatch",
    "virtual_reserve_config",
    "wrong_tx_association",
    "publication_ordering",
    "decode_apply_bug",
    "other",
)

DLMM_CLASSES = (
    "active_id",
    "volatility",
    "mm_bin_xy",
    "missing_bin",
    "publication_ordering",
    "order_inventory_related",
    "wrong_tx_association",
    "other",
)


def _i(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _role(w: dict) -> str:
    return str(w.get("role") or "")


def pump_class(b: dict) -> str:
    bucket = b.get("bucket") or ""
    fields = set(b.get("fields") or [])
    if bucket == "wrong_transaction_association":
        return "wrong_tx_association"
    if bucket == "trigger_decode_error":
        return "decode_apply_bug"
    if bucket in ("fork_order", "publication_incomplete", "missing_transaction_local_write"):
        return "publication_ordering"
    if bucket == "state_missing":
        return "publication_ordering"

    sp = b.get("s_prime") or {}
    sb = b.get("s_before") or {}
    pu = b.get("published_s") or b.get("s_pub") or {}
    staged = b.get("staged_writes") or b.get("staged") or []

    if not isinstance(sp, dict) or not isinstance(pu, dict):
        return "decode_apply_bug"
    if pu.get("note") and "reserve_base" not in pu:
        return "wrong_tx_association"

    prb, prq = _i(sp.get("reserve_base")), _i(sp.get("reserve_quote"))
    urb, urq = _i(pu.get("reserve_base")), _i(pu.get("reserve_quote"))
    brb, brq = _i(sb.get("reserve_base")) if isinstance(sb, dict) else None, (
        _i(sb.get("reserve_quote")) if isinstance(sb, dict) else None
    )
    vq = _i(sp.get("virtual_quote"))
    if vq is None and isinstance(sb, dict):
        vq = _i(sb.get("virtual_quote"))

    vault_staged = any("vault" in _role(w) for w in staged if isinstance(w, dict))
    pub_eq_before = brb is not None and brb == urb and brq == urq
    pred_eq_before = brb is not None and brb == prb and brq == prq

    # Publication never carries virtual_quote; apply blob pins it to 0.
    # Vault token amounts are published as reserve_*. That is the systematic hole.
    if (vq == 0 or vq is None) and prb is not None and urb is not None:
        if prb != urb or prq != urq:
            if not pub_eq_before:
                return "virtual_reserve_config"

    if vault_staged and pub_eq_before and not pred_eq_before:
        return "vault_mismatch"
    if "reserves" in fields or (prb is not None and urb is not None and (prb != urb or prq != urq)):
        return "reserve_mismatch"
    return "other"


def dlmm_class(b: dict) -> str:
    bucket = b.get("bucket") or ""
    fields = list(b.get("fields") or [])
    fset = set(fields)
    if bucket == "wrong_transaction_association":
        return "wrong_tx_association"
    if bucket in ("fork_order", "publication_incomplete", "missing_transaction_local_write"):
        return "publication_ordering"
    if bucket == "bin_traversal_account_coverage":
        return "missing_bin"

    sp = b.get("s_prime") or {}
    pu = b.get("published_s") or b.get("s_pub") or {}
    bins = (pu.get("bins") or {}) if isinstance(pu, dict) else {}
    missing = False
    xy_mis = False
    for row in (sp.get("touched") or []) if isinstance(sp, dict) else []:
        bid = row.get("id")
        xy = bins.get(bid)
        if xy is None:
            xy = bins.get(str(bid))
        if xy is None:
            missing = True
        elif tuple(xy) != (int(row["x"]), int(row["y"])):
            xy_mis = True
    if missing:
        return "missing_bin"
    if "active_id" in fset:
        return "active_id"
    if fset <= {"volatility", "fee_state"} or bucket == "fee_volatility_timing":
        return "volatility"
    if xy_mis or "bin_liquidity" in fset:
        # MM-only kernel vs open/processed orders: bin x/y moves without active_id.
        if "active_id" not in fset:
            return "order_inventory_related"
        return "mm_bin_xy"
    if bucket == "state_missing":
        return "publication_ordering"
    return "other"


def field_sig(b: dict) -> str:
    return ",".join(sorted(set(b.get("fields") or [])))


def cluster_key(kind: str, cls: str, b: dict) -> str:
    return f"{kind}|{cls}|{b.get('bucket')}|{field_sig(b)}"


def load_shadow(path: Path) -> dict:
    exact = Counter()
    mis = Counter()
    n = 0
    if not path.exists():
        return {"rows": 0, "exact": {}, "mismatch": {}}
    with path.open(encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            n += 1
            k = r.get("kind") or "unknown"
            if r.get("class") == "exact":
                exact[k] += 1
            elif r.get("class") == "mismatch":
                mis[k] += 1
    return {"rows": n, "exact": dict(exact), "mismatch": dict(mis)}


def reduce(root: Path) -> dict:
    shadow = load_shadow(root / "SHADOW.jsonl")
    misdir = root / "mismatch"
    pump = Counter()
    dlmm = Counter()
    clusters = Counter()
    examples = {}
    n_pump = n_dlmm = 0
    scored = 0

    files = list(misdir.glob("*.json")) if misdir.is_dir() else []
    for p in files:
        try:
            b = json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            continue
        kind = b.get("kind") or "unknown"
        if kind == "unknown" and (b.get("bucket") == "state_missing"):
            continue
        if kind == "pump":
            cls = pump_class(b)
            pump[cls] += 1
            n_pump += 1
        elif kind == "dlmm":
            cls = dlmm_class(b)
            dlmm[cls] += 1
            n_dlmm += 1
        else:
            continue
        scored += 1
        ck = cluster_key(kind, cls, b)
        clusters[ck] += 1
        if ck not in examples:
            examples[ck] = {
                "file": str(p),
                "sig": b.get("trigger_sig"),
                "idx": b.get("idx"),
                "pool": b.get("pool"),
                "slot": b.get("slot"),
                "direction": b.get("direction"),
                "amount_in": b.get("amount_in"),
                "bucket": b.get("bucket"),
                "fields": b.get("fields"),
                "s_before": b.get("s_before"),
                "n": {"direction": b.get("direction"), "amount_in": b.get("amount_in")},
                "s_prime": b.get("s_prime"),
                "published_s": b.get("published_s") or b.get("s_pub"),
                "staged_writes": (b.get("staged_writes") or [])[:8],
                "n_ix": b.get("n_ix"),
            }

    def pct_board(counts: Counter, n: int, keys: tuple[str, ...]) -> dict:
        out = {}
        for k in keys:
            c = counts.get(k, 0)
            out[k] = {"n": c, "pct": (100.0 * c / n) if n else 0.0}
        extra = sum(v for k, v in counts.items() if k not in keys)
        if extra:
            out["unclassified"] = {"n": extra, "pct": 100.0 * extra / n if n else 0.0}
        return out

    top = clusters.most_common(20)
    pump_n = n_pump or 1
    dominant = None
    if n_pump:
        top_p = pump.most_common(1)[0]
        dominant = {"class": top_p[0], "n": top_p[1], "pct": 100.0 * top_p[1] / pump_n}

    return {
        "root": str(root),
        "shadow": shadow,
        "mismatch_files": len(files),
        "scored_associated": scored,
        "pump": {"n": n_pump, "board": pct_board(pump, n_pump, PUMP_CLASSES), "dominant": dominant},
        "dlmm": {"n": n_dlmm, "board": pct_board(dlmm, n_dlmm, DLMM_CLASSES)},
        "clusters": [{"key": k, "n": n} for k, n in top],
        "examples": {k: examples[k] for k, _ in top[:8]},
    }


def render_md(doc: dict) -> str:
    p, d = doc["pump"], doc["dlmm"]
    lines = [
        "# FASTSOAK failure reducer",
        "",
        f"root: `{doc['root']}`",
        f"SHADOW rows: {doc['shadow']['rows']}  exact={doc['shadow']['exact']}  mismatch={doc['shadow']['mismatch']}",
        f"mismatch files: {doc['mismatch_files']}  scored (pump+dlmm, skip unknown/state_missing): {doc['scored_associated']}",
        "",
        "## PUMP",
        f"n={p['n']}",
    ]
    if p.get("dominant"):
        dom = p["dominant"]
        lines.append(f"dominant: **{dom['class']} {dom['pct']:.1f}%** ({dom['n']})")
    lines.append("")
    for k, v in p["board"].items():
        lines.append(f"- {k}: {v['n']} ({v['pct']:.1f}%)")
    lines += ["", "## DLMM", f"n={d['n']}", ""]
    for k, v in d["board"].items():
        lines.append(f"- {k}: {v['n']} ({v['pct']:.1f}%)")
    lines += ["", "## top clusters", ""]
    for c in doc["clusters"][:12]:
        lines.append(f"- {c['n']}  `{c['key']}`")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "/data/bsc/captures/soak_fastsoak_20260925/state008")
    outdir = Path(sys.argv[2]) if len(sys.argv) > 2 else root
    doc = reduce(root)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "SCOREBOARD.json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    (outdir / "SCOREBOARD.md").write_text(render_md(doc), encoding="utf-8")
    print(render_md(doc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
