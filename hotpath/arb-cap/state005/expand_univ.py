#!/usr/bin/env python3
"""TRACK B — rank unknown OrbitFlare DLMM N and publish a new universe generation.

Off hot path. DLMM+Pump only. reserve_* = bin-sum. No send.
Rank = trigger_frequency × has_route0_partner. Atomic file publish.
"""
from __future__ import annotations

import json
import os
import struct
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

SEEN = Path("/home/louis/captures/state005/seen_n.jsonl")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT_DIR = Path("/home/louis/captures/paper_orbit")
MAX_POOLS = 65535
MAX_PUMP_PER_DLMM = 2
STABLE = {
    "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",  # USDC
    "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB",  # USDT
    "USD1ttGY1N17NEEHLmELoaybftRBUSErhqYiQzvEmuB",
    "2b1kV6DkPAnxd5ixfnxCpjxmKwqjjaYmCZfHsFu24GXo",  # PYUSD
}


def gpa_pump_for_token(token: str) -> list[str]:
    found: list[str] = []
    # Route0: Pump.base == TOKEN (offset 43), Pump.quote == WSOL (offset 75).
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
        )
    except Exception as e:
        print(f"  gpa pump {token[:8]} {type(e).__name__}", flush=True)
        return []
    for acc in res or []:
        pk = acc.get("pubkey")
        if pk:
            found.append(pk)
    return list(dict.fromkeys(found))


def read_existing_bin(path: Path) -> tuple[list[dict], int]:
    """Keep current generation records as raw blobs. No RPC."""
    b = path.read_bytes()
    magic, ver, n = struct.unpack_from("<IHH", b, 0)
    if magic != live.LIVE_MAGIC or ver not in (live.LIVE_VER, 2) or n == 0:
        raise SystemExit(f"bad liveuniv {path}")
    slot = struct.unpack_from("<Q", b, 8)[0]
    off = 24
    recs = []
    for _ in range(n):
        start = off
        proto = b[off]
        pk = live.b58e(b[off + 1 : off + 33])
        mx = b[off + 33 : off + 65]
        my = b[off + 65 : off + 97]
        off += 161
        if proto == live.PROTO_DLMM:
            nbin = struct.unpack_from("<H", b, off + 71)[0]
            off += 73 + nbin * 20
        elif proto == live.PROTO_PUMP:
            off += 50
        else:
            raise SystemExit(f"unsupported proto {proto} in {path}")
        mx_s, my_s = live.b58e(mx), live.b58e(my)
        token = mx_s if my_s == live.SOL else my_s
        recs.append(
            {
                "kind": "dlmm" if proto == live.PROTO_DLMM else "pump",
                "pk": pk,
                "raw": b[start:off],
                "token": token,
                "mx": mx_s,
                "my": my_s,
                "sol_side": mx_s == live.SOL or my_s == live.SOL,
                "slot": slot,
            }
        )
    return recs, slot


def route0_dlmm_ok(mx: str, my: str) -> bool:
    return my == live.SOL and mx != live.SOL


def route0_pump_ok(mx: str, my: str) -> bool:
    return my == live.SOL and mx != live.SOL


def filter_existing_route0(existing: list[dict]) -> list[dict]:
    """Keep only venues that can satisfy the route0 mint contract."""
    dlmms = [
        e
        for e in existing
        if e["kind"] == "dlmm" and route0_dlmm_ok(e["mx"], e["my"])
    ]
    tokens = {e["token"] for e in dlmms}
    pumps = [
        e
        for e in existing
        if e["kind"] == "pump"
        and route0_pump_ok(e["mx"], e["my"])
        and e["token"] in tokens
    ]
    dropped = len(existing) - len(dlmms) - len(pumps)
    print(
        f"filter existing {len(existing)} -> dlmm={len(dlmms)} pump={len(pumps)} "
        f"dropped={dropped}",
        flush=True,
    )
    return dlmms + pumps


def load_pool_class() -> dict:
    p = OUT_DIR / "POOL_CLASS.json"
    if not p.exists():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def save_pool_class(cls: dict) -> None:
    (OUT_DIR / "POOL_CLASS.json").write_text(
        json.dumps(cls, indent=2) + "\n", encoding="utf-8"
    )


def write_univ(items: list, out: Path, slot: int) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")
    with tmp.open("wb") as f:
        live.write_u32(f, live.LIVE_MAGIC)
        live.write_u16(f, live.LIVE_VER)
        live.write_u16(f, len(items))
        live.write_u64(f, slot)
        live.write_u64(f, slot if slot else 1)
        for it in items:
            if it.get("raw"):
                f.write(it["raw"])
                continue
            kind, r = it["kind"], it["row"]
            proto = live.PROTO_DLMM if kind == "dlmm" else live.PROTO_PUMP
            live.write_meta(
                f, proto, r["pubkey"], r["mint_x"], r["mint_y"], r["vault_x"], r["vault_y"]
            )
            if kind == "dlmm":
                live.write_dlmm(f, r["snap"])
            else:
                live.write_pump(f, r)
    tmp.replace(out)


def coverage(want: set[str], seen: Path) -> dict:
    n = known = 0
    if not seen.exists():
        return {"decoded": 0, "known": 0, "pct": 0.0}
    for line in seen.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        n += 1
        pk = d._pk(bytes.fromhex(row["pool"]))
        if pk in want:
            known += 1
    return {"decoded": n, "known": known, "pct": (100.0 * known / n) if n else 0.0}


def main() -> int:
    acquire_gen_lock()
    try:
        return _main()
    finally:
        release_gen_lock()


def _main() -> int:
    live.load_dotenv()
    seen = Path(sys.argv[1] if len(sys.argv) > 1 else SEEN)
    univ_js = Path(sys.argv[2] if len(sys.argv) > 2 else UNIV)
    out_bin = Path(sys.argv[3] if len(sys.argv) > 3 else OUT_DIR / "liveuniv.bin")
    meta = json.loads(univ_js.read_text(encoding="utf-8"))
    have = {p["pubkey"] for p in meta.get("pools") or []}
    freq: Counter[str] = Counter()
    last_seen: dict[str, int] = {}
    for line in seen.read_text(encoding="utf-8").splitlines() if seen.exists() else []:
        if not line.strip():
            continue
        row = json.loads(line)
        pk = d._pk(bytes.fromhex(row["pool"]))
        last_seen[pk] = int(row.get("slot") or 0)
        if pk not in have:
            freq[pk] += 1
    unknown = [
        {"pool": pk, "protocol": "dlmm", "n": c, "last_seen": last_seen.get(pk, 0)}
        for pk, c in freq.most_common()
    ]
    (OUT_DIR / "UNKNOWN_POOLS.json").write_text(
        json.dumps({"n": len(unknown), "rows": unknown[:400]}, indent=2) + "\n"
    )
    print(
        f"unknown_dlmm={len(unknown)} top={[(u['pool'][:8], u['n']) for u in unknown[:8]]}",
        flush=True,
    )

    filter_only = os.environ.get("FILTER_ONLY") == "1" or "--filter-only" in sys.argv
    fees = (20, 5, 0, 0)
    if not filter_only:
        gcfg_pk = d._pk(d.find_pda([b"global_config"], live.b58d(live.PUMP)))
        gacc = d.get_multiple([gcfg_pk])[0]
        fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)

    cur_bin = out_bin if out_bin.exists() else Path(str(meta.get("path") or out_bin))
    existing, old_slot = read_existing_bin(cur_bin)
    pool_class = load_pool_class()
    items: list = filter_existing_route0(existing)
    pump_tokens = {e["token"] for e in items if e["kind"] == "pump"}
    for e in existing:
        if e["kind"] != "dlmm":
            continue
        if not route0_dlmm_ok(e["mx"], e["my"]):
            pool_class[e["pk"]] = "dlmm_y_not_wsol"
        elif e["token"] in pump_tokens:
            pool_class[e["pk"]] = "compatible"
    seen_pk: set[str] = {e["pk"] for e in items}
    print(f"keep existing n={len(items)} slot={old_slot}", flush=True)

    added_dlmm = 0
    added_pump = 0
    ranked = []
    if filter_only:
        unknown = []
        print("FILTER_ONLY — no RPC expand", flush=True)
    for u in unknown:
        if len(items) >= MAX_POOLS - 2:
            break
        pk = u["pool"]
        if pk in seen_pk:
            continue
        time.sleep(0.35)
        row = fetch_priced(pk)
        if row is None:
            ranked.append({**u, "partner": False, "score": 0, "reason": "no_snap_or_not_sol"})
            pool_class[pk] = "no_snap_or_not_sol"
            continue
        if live.b58e(row["mint_y"]) != live.SOL or live.b58e(row["mint_x"]) == live.SOL:
            ranked.append({**u, "partner": False, "score": 0, "reason": "dlmm_y_not_wsol"})
            pool_class[pk] = "dlmm_y_not_wsol"
            print(f"  skip {pk[:8]} n={u['n']} DLMM Y!=WSOL", flush=True)
            continue
        if row["token"] in STABLE or row["token"] == live.SOL:
            ranked.append({**u, "partner": False, "score": 0, "reason": "stable_or_sol"})
            pool_class[pk] = "stable_or_sol"
            print(f"  skip {pk[:8]} n={u['n']} stable {row['token'][:8]}", flush=True)
            continue
        pumps = gpa_pump_for_token(row["token"])
        if not pumps:
            ranked.append({**u, "partner": False, "score": 0, "reason": "no_pump"})
            pool_class[pk] = "no_pump"
            print(f"  skip {pk[:8]} n={u['n']} no pump", flush=True)
            continue
        ranked.append({**u, "partner": True, "score": u["n"], "pumps": pumps[:MAX_PUMP_PER_DLMM]})
        pool_class[pk] = "compatible"
        items.append({"kind": "dlmm", "row": row, "pk": live.b58e(row["pubkey"])})
        seen_pk.add(pk)
        added_dlmm += 1
        print(
            f"  add dlmm {pk[:8]} n={u['n']} token={row['token'][:8]} pumps={len(pumps)}",
            flush=True,
        )
        for ppk in pumps[:MAX_PUMP_PER_DLMM]:
            if len(items) >= MAX_POOLS:
                break
            if ppk in seen_pk:
                continue
            time.sleep(0.2)
            pr = live.fetch_pump(ppk, fees)
            if pr is None:
                continue
            items.append({"kind": "pump", "row": pr, "pk": ppk})
            seen_pk.add(ppk)
            added_pump += 1

    (OUT_DIR / "UNKNOWN_RANKED.json").write_text(
        json.dumps({"n": len(ranked), "rows": ranked[:400]}, indent=2) + "\n"
    )
    save_pool_class(pool_class)

    want = set(seen_pk)
    cov = coverage(want, seen)
    gen_path = OUT_DIR / "UNIV_GEN"
    gen = int(gen_path.read_text().strip() or "0") + 1 if gen_path.exists() else 1
    gen_bin = OUT_DIR / f"liveuniv.gen{gen}.bin"
    new_slot = max(
        [old_slot]
        + [int(it.get("slot") or 0) for it in items]
        + [int((it.get("row") or {}).get("slot") or 0) for it in items],
    )
    write_univ(items, gen_bin, new_slot)
    js = {
        "path": str(out_bin),
        "gen": gen,
        "gen_path": str(gen_bin),
        "n": len(items),
        "n_dlmm": sum(1 for it in items if it.get("kind") == "dlmm"),
        "n_pump": sum(1 for it in items if it.get("kind") == "pump"),
        "added_dlmm": added_dlmm,
        "added_pump": added_pump,
        "slot": new_slot,
        "reserve": "bin-sum",
        "coverage_seen_n": cov,
        "unknown_considered": len(unknown),
        "unknown_with_partner": sum(1 for r in ranked if r.get("partner")),
        "pools": [
            {
                "idx": i,
                "proto": it.get("kind"),
                "pubkey": it.get("pk") or live.b58e((it.get("row") or {}).get("pubkey") or b""),
                "sol_side": it.get("sol_side", (it.get("row") or {}).get("sol_side")),
                "token": it.get("token") or (it.get("row") or {}).get("token"),
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
        f"added_d={added_dlmm} added_p={added_pump} "
        f"coverage {cov['known']}/{cov['decoded']}={cov['pct']:.1f}%",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
