#!/usr/bin/env python3
"""GEN10 — fill remaining LIVE_POOL_MAX slots from FRAMED unknowns.

Score = framed_frequency × closed_routes × exec_support.
DLMM+Pump only. No CPMM GPA. Does not touch #6. No send.
"""
from __future__ import annotations

import json
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
from rebuild_univ import fetch_priced  # noqa: E402
from univ_gen_lock import acquire as acquire_gen_lock  # noqa: E402
from univ_gen_lock import release as release_gen_lock  # noqa: E402

from expand_framed import (  # noqa: E402
    MAX_POOLS,
    OUT_DIR,
    STABLE,
    UNIV,
    closes_sol,
    gpa_pump_token_sol,
    read_existing_bin,
    token_of,
    write_univ,
)
from typed_score import delta_closed, exec_support  # noqa: E402

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
MAX_FETCH = 110
MAX_PUMP_PER = 2


def live_unknown() -> list[dict]:
    freq: Counter[str] = Counter()
    proto: dict[str, str] = {}
    if not AUDIT.exists():
        return []
    with AUDIT.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if '"unknown_pool"' not in line and '"framed_pool"' not in line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = rec.get("kind")
            if kind not in ("unknown_pool", "framed_pool"):
                continue
            hx = rec.get("pool_hex") or ""
            if len(hx) < 64:
                continue
            try:
                pk = d._pk(bytes.fromhex(hx[:64]))
            except Exception:
                continue
            if kind == "unknown_pool":
                freq[pk] += 1
                pr = int(rec.get("proto") or 0)
                proto[pk] = "dlmm" if pr == 1 else "pump" if pr == 2 else proto.get(pk, "")
    return [{"pool": pk, "n": n, "proto": proto.get(pk, "")} for pk, n in freq.most_common()]


def as_edge(it: dict) -> dict:
    row = it.get("row") or {}
    mx = it.get("mx") or live.b58e(row.get("mint_x") or b"")
    my = it.get("my") or live.b58e(row.get("mint_y") or b"")
    return {"kind": it.get("kind"), "mx": mx, "my": my, "pk": it.get("pk")}


def main() -> int:
    acquire_gen_lock()
    try:
        return _main()
    finally:
        release_gen_lock()


def _main() -> int:
    live.load_dotenv()
    univ_js = Path(sys.argv[1] if len(sys.argv) > 1 else UNIV)
    out_bin = Path(sys.argv[2] if len(sys.argv) > 2 else OUT_DIR / "liveuniv.bin")
    meta = json.loads(univ_js.read_text(encoding="utf-8"))
    have = {p["pubkey"] for p in meta.get("pools") or []}
    unknown = [u for u in live_unknown() if u["pool"] not in have]
    print(f"GEN10  have={len(have)} unknown_framed={len(unknown)} slots={MAX_POOLS - len(have)}", flush=True)

    cur_bin = out_bin if out_bin.exists() else Path(str(meta.get("path") or out_bin))
    existing, old_slot, _ver = read_existing_bin(cur_bin)
    items = list(existing)
    seen_pk = {e["pk"] for e in items}
    base_edges = [as_edge(e) for e in items]
    room = MAX_POOLS - len(items)
    if room <= 0:
        print("GEN10  full", flush=True)
        return 0

    gcfg_pk = d._pk(d.find_pda([b"global_config"], live.b58d(live.PUMP)))
    gacc = d.get_multiple([gcfg_pk], retries=2)[0]
    fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)
    tokens_sol = {e["token"] for e in items if e.get("kind") == "pump" and e.get("my") == live.SOL}

    scored: list[dict] = []
    for u in unknown[:MAX_FETCH]:
        if len(scored) >= 80:
            break
        pk = u["pool"]
        if pk in seen_pk:
            continue
        time.sleep(0.12)
        kind = u.get("proto") or "dlmm"
        extra: list[dict] = []
        if kind == "pump":
            pr = live.fetch_pump(pk, fees)
            if pr is None or live.b58e(pr["mint_y"]) != live.SOL:
                continue
            extra.append({"kind": "pump", "row": pr, "pk": pk, "token": pr["token"],
                          "mx": live.b58e(pr["mint_x"]), "my": live.b58e(pr["mint_y"])})
        else:
            row = fetch_priced(pk)
            if row is None:
                print(f"  skip {pk[:8]} n={u['n']} no_snap", flush=True)
                continue
            mx, my = live.b58e(row["mint_x"]), live.b58e(row["mint_y"])
            tok = token_of(mx, my)
            sol_stable = closes_sol(mx, my) and (mx in STABLE or my in STABLE)
            token_sol = closes_sol(mx, my) and tok not in STABLE
            token_stable = (mx in STABLE or my in STABLE) and tok not in STABLE and not closes_sol(mx, my)
            if not (sol_stable or token_sol or token_stable):
                print(f"  skip {pk[:8]} n={u['n']} no_edge", flush=True)
                continue
            extra.append({"kind": "dlmm", "row": row, "pk": pk, "token": tok, "mx": mx, "my": my})
            if not sol_stable:
                for ppk in gpa_pump_token_sol(tok)[:MAX_PUMP_PER]:
                    if ppk in seen_pk or any(x["pk"] == ppk for x in extra):
                        continue
                    time.sleep(0.08)
                    pr = live.fetch_pump(ppk, fees)
                    if pr is None:
                        continue
                    extra.append({"kind": "pump", "row": pr, "pk": ppk, "token": pr["token"],
                                  "mx": live.b58e(pr["mint_x"]), "my": live.b58e(pr["mint_y"])})
            if token_stable and tok not in tokens_sol and not any(x["kind"] == "pump" for x in extra):
                print(f"  skip {pk[:8]} n={u['n']} no_pump_sol", flush=True)
                continue
        dlt = delta_closed(base_edges, [as_edge(x) for x in extra])
        es = exec_support(dlt)
        n_closed = dlt["closed"]
        if n_closed <= 0 or es <= 0:
            print(f"  skip {pk[:8]} n={u['n']} isolated closed=0", flush=True)
            continue
        score = float(u["n"]) * n_closed * es
        scored.append({"score": score, "freq": u["n"], "delta": dlt, "exec": es, "extra": extra})
        print(
            f"  cand {pk[:8]} n={u['n']} closed=+{n_closed} "
            f"r0=+{dlt['route0']} hop3=+{dlt['n3']} exec={es} score={score:.0f} slots={len(extra)}",
            flush=True,
        )

    scored.sort(key=lambda r: -r["score"])
    added_dlmm = added_pump = 0
    picked = []
    for row in scored:
        extra = row["extra"]
        if len(items) + len(extra) > MAX_POOLS:
            extra = [x for x in extra if x["pk"] not in seen_pk][: MAX_POOLS - len(items)]
        extra = [x for x in extra if x["pk"] not in seen_pk]
        if not extra:
            continue
        for x in extra:
            items.append(x)
            seen_pk.add(x["pk"])
            if x["kind"] == "dlmm":
                added_dlmm += 1
            else:
                added_pump += 1
                if x.get("my") == live.SOL:
                    tokens_sol.add(x.get("token") or "")
        picked.append(row)
        print(f"  ADD score={row['score']:.0f} {[x['pk'][:8] for x in extra]}", flush=True)
        if len(items) >= MAX_POOLS:
            break

    new_slot = max(
        [old_slot]
        + [int(it.get("slot") or 0) for it in items]
        + [int((it.get("row") or {}).get("slot") or 0) for it in items],
    )
    gen_path = OUT_DIR / "UNIV_GEN"
    gen = int(gen_path.read_text().strip() or "0") + 1 if gen_path.exists() else 1
    gen_bin = OUT_DIR / f"liveuniv.gen{gen}.bin"
    write_univ(items, gen_bin, new_slot)
    js = {
        "path": str(out_bin),
        "gen": gen,
        "gen_path": str(gen_bin),
        "n": len(items),
        "n_dlmm": sum(1 for it in items if it.get("kind") == "dlmm"),
        "n_pump": sum(1 for it in items if it.get("kind") == "pump"),
        "n_cpmm": sum(1 for it in items if it.get("kind") == "cpmm"),
        "added_dlmm": added_dlmm,
        "added_pump": added_pump,
        "rank": "freq*closed*exec",
        "picked": [
            {"score": r["score"], "freq": r["freq"], "delta": r["delta"], "exec": r["exec"],
             "pks": [x["pk"] for x in r["extra"]]}
            for r in picked
        ],
        "slot": new_slot,
        "compiler": "typed-2-3hop + route0 + fam5",
        "pools": [
            {
                "idx": i,
                "proto": it.get("kind"),
                "pubkey": it.get("pk") or live.b58e((it.get("row") or {}).get("pubkey") or b""),
                "token": it.get("token") or (it.get("row") or {}).get("token"),
                "mx": it.get("mx"),
                "my": it.get("my"),
            }
            for i, it in enumerate(items)
        ],
    }
    (OUT_DIR / "liveuniv.json").write_text(json.dumps(js, indent=2) + "\n")
    (OUT_DIR / "COVERAGE.json").write_text(json.dumps(js, indent=2) + "\n")
    (OUT_DIR / "GEN10_RANK.json").write_text(json.dumps(js["picked"], indent=2) + "\n")
    gen_path.write_text(str(gen) + "\n")
    tmp = out_bin.with_suffix(".bin.tmp")
    tmp.write_bytes(gen_bin.read_bytes())
    tmp.replace(out_bin)
    print(
        f"GEN {gen} n={js['n']} dlmm={js['n_dlmm']} pump={js['n_pump']} "
        f"added_d={added_dlmm} added_p={added_pump} picked={len(picked)}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
