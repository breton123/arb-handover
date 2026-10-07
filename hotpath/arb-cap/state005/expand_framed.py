#!/usr/bin/env python3
"""Next immutable universe from FRAMED rank + typed DLMM/Pump/CPMM cycles.

Does not mutate route0. Inverted DLMM+Pump become family 5 at compile.
Ray CPMM WSOL↔USDC/USDT are the third edge. LIVE_POOL_MAX stays 256
(paper.c stack-copies live_univ_t). Off hot path. No send.
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
from rebuild_univ import fetch_priced  # noqa: E402
from univ_gen_lock import acquire as acquire_gen_lock  # noqa: E402
from univ_gen_lock import release as release_gen_lock  # noqa: E402

UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT_DIR = Path("/home/louis/captures/paper_orbit")
RANK = OUT_DIR / "FRAMED_POOLS.json"
SEEN_N = Path("/home/louis/captures/state005/seen_n.jsonl")
MAX_POOLS = 65535
MAX_NEW_DLMM = 36
MAX_PUMP_PER = 2
STABLE = {live.USDC, live.USDT}


def closes_sol(mx: str, my: str) -> bool:
    return live.SOL in (mx, my) and mx != my


def token_of(mx: str, my: str) -> str:
    if my == live.SOL:
        return mx
    if mx == live.SOL:
        return my
    if my in STABLE:
        return mx
    if mx in STABLE:
        return my
    return mx


def gpa_pump_token_sol(token: str) -> list[str]:
    found: list[str] = []
    try:
        res = d.rpc(
            "getProgramAccounts",
            [
                live.PUMP,
                {
                    "encoding": "base64",
                    "filters": [
                        {"memcmp": {"offset": 43, "bytes": token}},
                        {"memcmp": {"offset": 75, "bytes": live.SOL}},
                    ],
                },
            ],
            retries=2,
            backoff=2.0,
        )
    except Exception as e:
        print(f"  gpa pump {token[:8]} {type(e).__name__}", flush=True)
        return []
    for acc in res or []:
        pk = acc.get("pubkey")
        if pk:
            found.append(pk)
    return list(dict.fromkeys(found))


def gpa_cpmm(mint0: str, mint1: str) -> list[str]:
    try:
        res = d.rpc(
            "getProgramAccounts",
            [
                live.CPMM,
                {
                    "encoding": "base64",
                    "filters": [
                        {"memcmp": {"offset": 72, "bytes": mint0}},
                        {"memcmp": {"offset": 104, "bytes": mint1}},
                    ],
                },
            ],
            retries=2,
            backoff=2.0,
        )
    except Exception as e:
        print(f"  gpa cpmm {mint0[:6]}/{mint1[:6]} {type(e).__name__}", flush=True)
        return []
    return [a["pubkey"] for a in (res or []) if a.get("pubkey")]


def discover_stable_cpmm() -> list[str]:
    out: list[str] = []
    for stable in (live.USDC, live.USDT):
        for a, b in ((live.SOL, stable), (stable, live.SOL)):
            out.extend(gpa_cpmm(a, b))
    return list(dict.fromkeys(out))


def read_existing_bin(path: Path) -> tuple[list[dict], int, int]:
    b = path.read_bytes()
    magic, ver, n = struct.unpack_from("<IHH", b, 0)
    if magic != live.LIVE_MAGIC or ver not in (live.LIVE_VER, live.LIVE_VER2) or n == 0:
        raise SystemExit(f"bad liveuniv {path}")
    slot = struct.unpack_from("<Q", b, 8)[0]
    off = 24
    recs = []
    for _ in range(n):
        start = off
        proto = b[off]
        pk = live.b58e(b[off + 1 : off + 33])
        mx = live.b58e(b[off + 33 : off + 65])
        my = live.b58e(b[off + 65 : off + 97])
        off += 161
        if proto == live.PROTO_DLMM:
            nbin = struct.unpack_from("<H", b, off + 71)[0]
            off += 73 + nbin * 20
            kind = "dlmm"
        elif proto == live.PROTO_PUMP:
            off += 50
            kind = "pump"
        elif proto == live.PROTO_CPMM:
            off += 19
            kind = "cpmm"
        else:
            raise SystemExit(f"unsupported proto {proto} in {path}")
        recs.append(
            {
                "kind": kind,
                "pk": pk,
                "raw": b[start:off],
                "token": token_of(mx, my),
                "mx": mx,
                "my": my,
                "sol_side": closes_sol(mx, my),
                "slot": slot,
            }
        )
    return recs, slot, ver


def write_univ(items: list, out: Path, slot: int) -> None:
    has_cpmm = any(it.get("kind") == "cpmm" for it in items)
    ver = live.LIVE_VER2 if has_cpmm else live.LIVE_VER
    tmp = out.with_suffix(out.suffix + ".tmp")
    with tmp.open("wb") as f:
        live.write_u32(f, live.LIVE_MAGIC)
        live.write_u16(f, ver)
        live.write_u16(f, len(items))
        live.write_u64(f, slot)
        live.write_u64(f, slot if slot else 1)
        for it in items:
            if it.get("raw") and it.get("kind") != "cpmm":
                f.write(it["raw"])
                continue
            kind, r = it["kind"], it.get("row") or {}
            if kind == "dlmm":
                live.write_meta(
                    f, live.PROTO_DLMM, r["pubkey"], r["mint_x"], r["mint_y"],
                    r["vault_x"], r["vault_y"],
                )
                live.write_dlmm(f, r["snap"])
            elif kind == "pump":
                live.write_meta(
                    f, live.PROTO_PUMP, r["pubkey"], r["mint_x"], r["mint_y"],
                    r["vault_x"], r["vault_y"],
                )
                live.write_pump(f, r)
            else:
                live.write_meta(
                    f, live.PROTO_CPMM, r["pubkey"], r["mint_x"], r["mint_y"],
                    r["vault_x"], r["vault_y"],
                )
                live.write_amm2(f, r)
    tmp.replace(out)


def load_rank() -> list[dict]:
    rows: list[dict] = []
    if RANK.exists():
        try:
            rows = json.loads(RANK.read_text(encoding="utf-8")).get("rows") or []
        except json.JSONDecodeError:
            rows = []
    have = {r.get("pool") for r in rows}
    n_dlmm = sum(1 for r in rows if r.get("proto") == "dlmm")
    if n_dlmm >= 12 or not SEEN_N.exists():
        return rows
    extra: dict[str, int] = {}
    for line in SEEN_N.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        hx = rec.get("pool") or ""
        if len(hx) < 64:
            continue
        try:
            pk = d._pk(bytes.fromhex(hx[:64]))
        except Exception:
            continue
        if pk in have:
            continue
        extra[pk] = extra.get(pk, 0) + 1
    add = [{"pool": pk, "n": n, "proto": "dlmm", "src": "seen_n"} for pk, n in
           sorted(extra.items(), key=lambda kv: -kv[1])[:80]]
    print(f"  rank supplement seen_n={len(add)} (framed dlmm={n_dlmm})", flush=True)
    return rows + add


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
    rank = load_rank()
    print(f"FRAMED expand rank={len(rank)} have={len(have)}", flush=True)

    cur_bin = out_bin if out_bin.exists() else Path(str(meta.get("path") or out_bin))
    existing, old_slot, _ver = read_existing_bin(cur_bin)
    items = list(existing)
    seen_pk = {e["pk"] for e in items}

    gcfg_pk = d._pk(d.find_pda([b"global_config"], live.b58d(live.PUMP)))
    gacc = d.get_multiple([gcfg_pk], retries=2)[0]
    fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)

    added_dlmm = added_pump = added_cpmm = 0
    print("discover CPMM WSOL↔USDC/USDT", flush=True)
    for pk in discover_stable_cpmm()[:4]:
        if pk in seen_pk or len(items) >= MAX_POOLS:
            continue
        row = live.fetch_cpmm(pk)
        if not row:
            continue
        mx, my = live.b58e(row["mint_x"]), live.b58e(row["mint_y"])
        if not ((mx == live.SOL and my in STABLE) or (my == live.SOL and mx in STABLE)):
            continue
        items.append({"kind": "cpmm", "row": row, "pk": pk, "token": row["token"], "mx": mx, "my": my})
        seen_pk.add(pk)
        added_cpmm += 1
        print(f"  add cpmm {pk[:8]} {mx[:6]}/{my[:6]}", flush=True)

    tokens_sol = {e["token"] for e in items if e.get("kind") == "pump" and e.get("my") == live.SOL}
    for e in items:
        if e.get("kind") == "pump" and e.get("my") == live.SOL:
            tokens_sol.add(e["token"])

    considered = 0
    for u in rank:
        if added_dlmm >= MAX_NEW_DLMM or len(items) >= MAX_POOLS - 4:
            break
        pk = u.get("pool") or ""
        if not pk or pk in seen_pk:
            continue
        considered += 1
        time.sleep(0.15)
        kind = u.get("proto") or ""
        if kind == "pump":
            pr = live.fetch_pump(pk, fees)
            if pr is None or live.b58e(pr["mint_y"]) != live.SOL:
                continue
            items.append({"kind": "pump", "row": pr, "pk": pk, "token": pr["token"],
                          "mx": live.b58e(pr["mint_x"]), "my": live.b58e(pr["mint_y"])})
            seen_pk.add(pk)
            tokens_sol.add(pr["token"])
            added_pump += 1
            continue
        row = fetch_priced(pk)
        if row is None:
            print(f"  skip {pk[:8]} n={u.get('n')} no_snap", flush=True)
            continue
        mx, my = live.b58e(row["mint_x"]), live.b58e(row["mint_y"])
        tok = token_of(mx, my)
        sol_stable = closes_sol(mx, my) and (mx in STABLE or my in STABLE)
        token_sol = closes_sol(mx, my) and tok not in STABLE
        token_stable = (mx in STABLE or my in STABLE) and tok not in STABLE and not closes_sol(mx, my)
        if not (sol_stable or token_sol or token_stable):
            print(f"  skip {pk[:8]} n={u.get('n')} no_sol_or_stable", flush=True)
            continue
        pumps = [] if sol_stable else gpa_pump_token_sol(tok)
        if token_stable and not pumps and tok not in tokens_sol:
            print(f"  skip {pk[:8]} n={u.get('n')} no_pump_sol", flush=True)
            continue
        if token_sol and not pumps and tok not in tokens_sol:
            print(f"  skip {pk[:8]} n={u.get('n')} no_pump", flush=True)
            continue
        items.append({"kind": "dlmm", "row": row, "pk": pk, "token": tok, "mx": mx, "my": my})
        seen_pk.add(pk)
        added_dlmm += 1
        print(
            f"  add dlmm {pk[:8]} n={u.get('n')} {mx[:6]}/{my[:6]} pumps={len(pumps)}",
            flush=True,
        )
        for ppk in pumps[:MAX_PUMP_PER]:
            if len(items) >= MAX_POOLS or ppk in seen_pk:
                continue
            time.sleep(0.1)
            pr = live.fetch_pump(ppk, fees)
            if pr is None:
                continue
            items.append({"kind": "pump", "row": pr, "pk": ppk, "token": pr["token"],
                          "mx": live.b58e(pr["mint_x"]), "my": live.b58e(pr["mint_y"])})
            seen_pk.add(ppk)
            tokens_sol.add(pr["token"])
            added_pump += 1

    if len(items) > MAX_POOLS:
        scored = []
        freq = {r["pool"]: int(r.get("n") or 0) for r in rank}
        for it in items:
            scored.append((freq.get(it["pk"], 0) + (50 if it.get("kind") == "cpmm" else 0), it))
        scored.sort(key=lambda x: -x[0])
        items = [it for _, it in scored[:MAX_POOLS]]
        seen_pk = {it["pk"] for it in items}

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
        "added_cpmm": added_cpmm,
        "framed_considered": considered,
        "slot": new_slot,
        "reserve": "bin-sum",
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
    gen_path.write_text(str(gen) + "\n")
    tmp = out_bin.with_suffix(".bin.tmp")
    tmp.write_bytes(gen_bin.read_bytes())
    tmp.replace(out_bin)
    print(
        f"GEN {gen} n={js['n']} dlmm={js['n_dlmm']} pump={js['n_pump']} "
        f"cpmm={js['n_cpmm']} added_d={added_dlmm} added_p={added_pump} "
        f"added_c={added_cpmm}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
