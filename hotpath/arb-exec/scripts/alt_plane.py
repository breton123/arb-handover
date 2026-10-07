#!/usr/bin/env python3
"""Control-plane ALT/ATA sync for every legal route0.

Hot path only looks up. This script may create/extend ALTs and ATAs.
Never called from an opportunity. Does not touch executor or search kernels.
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-cap"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
import exec_live002b as e  # noqa: E402
import oneshot_live as o  # noqa: E402

UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
REPORT = Path("/home/louis/arb-cap/oneshot/ALT_PLANE.json")
SEED = [
    "J6jmHvnetqmzmBxpWyKDYmWEnTU77Dmcd7Le2sLyAfwu",
    "A3y4efDfkbJGF4QJtScfQAwubm2BzpTauAbZ8ohAHMgn",
    "3iN6TXFH5nwhRhMYsY92muGfyey8aKkiSTVYXL8Bu7SG",
]
ALT_MAX = 256
# Keep native SOL for ONESHOT fees. WSOL wrap is already on the wallet.
RESERVE_LAMPORTS = 80_000_000


def known_pubkeys() -> list[str]:
    out = []
    for p in (e.DEPLOY / "alt.json", e.PLANE):
        if not p.exists():
            continue
        obj = json.loads(p.read_text(encoding="utf-8"))
        if obj.get("pubkey"):
            out.append(obj["pubkey"])
        for a in obj.get("alts") or []:
            if a.get("pubkey"):
                out.append(a["pubkey"])
    out.extend(SEED)
    seen: set[str] = set()
    uniq = []
    for pk in out:
        if pk not in seen:
            seen.add(pk)
            uniq.append(pk)
    return uniq


def register() -> list[dict]:
    alts = []
    for pk in known_pubkeys():
        addrs = e.alt_addresses(pk)
        if not addrs:
            print(f"  skip empty/missing {pk[:8]}", flush=True)
            continue
        alts.append({"pubkey": pk, "addresses": list(addrs)})
        print(f"  {pk[:8]} n={len(addrs)}", flush=True)
    e.publish_plane(alts, [])
    print(f"wrote {e.PLANE} alts={len(alts)}", flush=True)
    return alts


def route0_pairs() -> list[tuple[str, str, str]]:
    univ = json.loads(UNIV.read_text(encoding="utf-8"))
    pools = univ.get("pools") or []
    by_tok: dict[str, dict[str, list[dict]]] = {}
    for meta in pools:
        tok = meta.get("token")
        proto = str(meta.get("proto") or meta.get("kind") or "")
        if not tok or proto not in ("dlmm", "pump"):
            continue
        by_tok.setdefault(tok, {}).setdefault(proto, []).append(meta)
    pairs = []
    for tok, g in by_tok.items():
        for dlmm in g.get("dlmm") or []:
            for pump in g.get("pump") or []:
                if not dlmm.get("sol_side"):
                    continue
                pairs.append((dlmm["pubkey"], pump["pubkey"], tok))
    return pairs


def _get_many(pks: list[str]) -> dict[str, dict | None]:
    out: dict[str, dict | None] = {}
    uniq = []
    seen = set()
    for p in pks:
        if p and p not in seen:
            seen.add(p)
            uniq.append(p)
    for i in range(0, len(uniq), 80):
        chunk = uniq[i:i + 80]
        got = d.get_multiple(chunk)
        for j, pk in enumerate(chunk):
            out[pk] = got[j]
    return out


def resolve_all(wallet: str) -> list[dict]:
    pairs = route0_pairs()
    print(f"legal route0 pairs {len(pairs)} dirs {len(pairs) * 2}", flush=True)
    first = [p for t in pairs for p in t[:2]]
    accs = _get_many(first)
    rows = []
    extra = []
    for dlmm_pk, pump_pk, tok in pairs:
        dacc = accs.get(dlmm_pk)
        pacc = accs.get(pump_pk)
        if not dacc or not pacc:
            print(f"  missing acc {dlmm_pk[:8]}/{pump_pk[:8]}", flush=True)
            continue
        try:
            lb = d.parse_lbpair(dacc["data"])
        except Exception as ex:
            print(f"  lb parse {dlmm_pk[:8]} {ex}", flush=True)
            continue
        p = live.parse_pump_pool(pacc["data"])
        if not p:
            print(f"  pump parse {pump_pk[:8]}", flush=True)
            continue
        mint_x = d._pk(lb["token_x"])
        mint_y = d._pk(lb["token_y"])
        if mint_y != e.SOL:
            print(f"  skip Y!WSOL {dlmm_pk[:8]}", flush=True)
            continue
        active = int(lb["active_id"])
        active_arr = d.bin_array_index(active)
        neighbor = []
        for i in (active_arr, active_arr + 1, active_arr - 1, active_arr + 2, active_arr - 2):
            neighbor.append(d.bin_array_pda(dlmm_pk, i))
        extra.extend(neighbor)
        extra.append(mint_x)
        extra.append(d._pk(d.find_pda([b"oracle", d.b58decode(dlmm_pk)], d.b58decode(e.DLMM))))
        rows.append({
            "dlmm": dlmm_pk,
            "pump": pump_pk,
            "token": tok,
            "lb": lb,
            "p": p,
            "pacc": pacc,
            "mint_x": mint_x,
            "mint_y": mint_y,
            "active_id": active,
            "neighbor_bins": neighbor,
        })
    extra_accs = _get_many(extra + [e.ata(wallet, e.SOL, e.TOKENKEG)])
    our_exec = e.program_v3()
    routes = []
    for row in rows:
        mint_x = row["mint_x"]
        mint_info = extra_accs.get(mint_x)
        base_tok = (mint_info or {}).get("owner") or e.TOKEN2022
        if base_tok not in (e.TOKENKEG, e.TOKEN2022):
            print(f"  bad mint owner {mint_x[:8]}", flush=True)
            continue
        live_bins = [b for b in row["neighbor_bins"] if extra_accs.get(b)]
        if len(live_bins) < 2:
            print(f"  no bins {row['dlmm'][:8]}", flush=True)
            continue
        c_auth, c_ata = o.creator_from_pool(row["pacc"]["data"], row["mint_y"])
        pump = e.pump_fallback()
        pump["creator_auth"] = c_auth
        pump["creator_ata"] = c_ata
        fee_recipient = e.FEE_RECIPIENT
        fee_rec_quote = e.ata(fee_recipient, row["mint_y"], e.TOKENKEG)
        acc = {
            "pair_dlmm": row["dlmm"],
            "pair_pump": row["pump"],
            "mint_x": mint_x,
            "mint_y": row["mint_y"],
            "base_tok": base_tok,
            "user_quote": e.ata(wallet, e.SOL, e.TOKENKEG),
            "user_base": e.ata(wallet, mint_x, base_tok),
            "event_dlmm": d._pk(d.find_pda([b"__event_authority"], d.b58decode(e.DLMM))),
            "oracle": d._pk(d.find_pda([b"oracle", d.b58decode(row["dlmm"])], d.b58decode(e.DLMM))),
            "bins": live_bins[:2],
            "vault_x": d._pk(row["lb"]["vault_x"]),
            "vault_y": d._pk(row["lb"]["vault_y"]),
            "vault_b": d._pk(row["p"]["vault_base"]),
            "vault_q": d._pk(row["p"]["vault_quote"]),
            "pump": pump,
            "fee_cfg": e.FEE_CFG_LIVE,
            "gvol": d._pk(d.find_pda([b"global_volume_accumulator"], d.b58decode(e.PUMP))),
            "pool_v2": d._pk(d.find_pda([b"pool-v2", d.b58decode(mint_x)], d.b58decode(e.PUMP))),
            "uvol": d._pk(d.find_pda(
                [b"user_volume_accumulator", d.b58decode(wallet)],
                d.b58decode(e.PUMP),
            )),
            "fee_recipient": fee_recipient,
            "fee_rec_quote": fee_rec_quote,
            "active_id": row["active_id"],
        }
        static, wr, ro = e.v0_parts(wallet, our_exec, acc)
        wr_buy = list(wr) + [acc["gvol"]]
        need = []
        for a in wr_buy + ro + live_bins:
            if a not in need:
                need.append(a)
        routes.append({
            "dlmm": row["dlmm"],
            "pump": row["pump"],
            "token": row["token"],
            "mint_x": mint_x,
            "base_tok": base_tok,
            "user_quote": acc["user_quote"],
            "user_base": acc["user_base"],
            "static": static,
            "wr": wr,
            "wr_buy": wr_buy,
            "ro": ro,
            "need": need,
            "active_id": row["active_id"],
        })
        print(f"  resolved {row['dlmm'][:8]}/{row['pump'][:8]} bins={len(live_bins)} need={len(need)}", flush=True)
    return routes


def greedy_pack(routes: list[dict], existing: list[dict]) -> list[dict]:
    packs = []
    for alt in existing:
        packs.append({
            "pubkey": alt["pubkey"],
            "frozen_n": len(alt["addresses"]),
            "planned": list(alt["addresses"]),
        })
    for route in sorted(routes, key=lambda r: -len(r["need"])):
        need = route["need"]
        placed = None
        for p in packs:
            extra = [a for a in need if a not in p["planned"]]
            if len(p["planned"]) + len(extra) <= ALT_MAX:
                p["planned"].extend(extra)
                placed = p
                break
        if placed is None:
            planned = []
            for a in need:
                if a not in planned:
                    planned.append(a)
            packs.append({"pubkey": None, "frozen_n": 0, "planned": planned})
            placed = packs[-1]
        route["pack"] = placed
    return packs


def spendable(wallet: str) -> int:
    return max(0, e.native_balance(wallet) - RESERVE_LAMPORTS)


def materialize_packs(payer, wallet: str, packs: list[dict]) -> None:
    for i, p in enumerate(packs):
        planned = p["planned"]
        if p.get("pubkey"):
            have = e.alt_addresses(p["pubkey"])
            extra = planned[len(have):]
            if extra:
                print(f"pack{i} extend {p['pubkey'][:8]} +{len(extra)} -> {len(have)+len(extra)}", flush=True)
                e.alt_extend(payer, p["pubkey"], extra)
                # wait until visible
                for _ in range(16):
                    got = e.alt_addresses(p["pubkey"])
                    if len(got) >= len(have) + len(extra):
                        p["addresses"] = got
                        break
                    time.sleep(0.4)
                else:
                    p["addresses"] = e.alt_addresses(p["pubkey"])
            else:
                p["addresses"] = have
        else:
            print(f"pack{i} create n={len(planned)}", flush=True)
            if spendable(wallet) < 6_000_000:
                print("  skip create — reserve", flush=True)
                p["addresses"] = []
                continue
            pk = e.alt_create(payer, planned)
            p["pubkey"] = pk
            p["addresses"] = e.alt_addresses(pk)
        live_alts = [x for x in packs if x.get("pubkey") and x.get("addresses")]
        e.publish_plane(live_alts, [])
        print(f"  published plane alts={len(live_alts)} last={p.get('pubkey','')[:8]} n={len(p.get('addresses') or [])}", flush=True)


def ensure_route_atas(payer, wallet: str, routes: list[dict]) -> None:
    e.ensure_ata(payer, wallet, e.SOL, e.TOKENKEG)
    for r in routes:
        if e.exists(r["user_base"]):
            r["ata_ok"] = True
            continue
        if spendable(wallet) < 3_000_000:
            print(f"  skip ATA {r['user_base'][:8]} reserve", flush=True)
            r["ata_ok"] = False
            continue
        try:
            e.ensure_ata(payer, wallet, r["mint_x"], r["base_tok"])
            r["ata_ok"] = e.exists(r["user_base"])
        except Exception as ex:
            print(f"  ATA fail {r['mint_x'][:8]} {ex}", flush=True)
            r["ata_ok"] = False


def compile_route(r: dict) -> dict:
    out = {"dir0": False, "dir1": False, "alt": None, "err": None}
    try:
        alt0, wr0, ro0 = e.lookup_prepared(r["wr"], r["ro"])
        tmpl0 = e.compile_v0(r["static"], alt0, wr0, ro0)
        out["dir0"] = True
        out["alt"] = alt0
        out["tmpl0"] = tmpl0.hex()
    except (e.AltPlaneMiss, SystemExit, RuntimeError, ValueError) as ex:
        out["err"] = f"dir0 {ex}"[:160]
    try:
        alt1, wr1, ro1 = e.lookup_prepared(r["wr_buy"], r["ro"])
        tmpl1 = e.compile_v0_buy(r["static"], alt1, wr1, ro1)
        out["dir1"] = True
        out["alt"] = alt1
        out["tmpl1"] = tmpl1.hex()
    except (e.AltPlaneMiss, SystemExit, RuntimeError, ValueError) as ex:
        out["err"] = (out.get("err") or "") + f" dir1 {ex}"[:160]
    return out


def sync() -> dict:
    live.load_dotenv()
    payer = e.payer_kp()
    wallet = str(payer.pubkey())
    print(f"ALT_PLANE sync wallet={wallet} native={e.native_balance(wallet)/1e9:.6f}", flush=True)
    existing = register()
    routes = resolve_all(wallet)
    packs = greedy_pack(routes, existing)
    print(
        f"packs {len(packs)} planned={[len(p['planned']) for p in packs]}",
        flush=True,
    )
    materialize_packs(payer, wallet, packs)
    ensure_route_atas(payer, wallet, routes)
    e.publish_plane(
        [p for p in packs if p.get("pubkey") and p.get("addresses")],
        [],
    )
    ready_rows = []
    n_ready = 0
    for r in routes:
        comp = compile_route(r)
        ata_ok = bool(r.get("ata_ok") or (e.exists(r["user_quote"]) and e.exists(r["user_base"])))
        race = bool(comp["dir0"] and comp["dir1"] and ata_ok)
        if race:
            n_ready += 1
        ready_rows.append({
            "dlmm": r["dlmm"],
            "pump": r["pump"],
            "token": r["token"],
            "alt": comp.get("alt"),
            "compile_dir0": comp["dir0"],
            "compile_dir1": comp["dir1"],
            "ata_ok": ata_ok,
            "RACE_READY": race,
            "tmpl0": comp.get("tmpl0") if race else None,
            "tmpl1": comp.get("tmpl1") if race else None,
            "err": comp.get("err"),
            "need_n": len(r["need"]),
        })
        print(
            f"  {'READY' if race else 'WAIT '} {r['dlmm'][:8]}/{r['pump'][:8]} "
            f"d0={comp['dir0']} d1={comp['dir1']} ata={ata_ok}",
            flush=True,
        )
    e.publish_plane(
        [p for p in packs if p.get("pubkey") and p.get("addresses")],
        ready_rows,
    )
    rep = {
        "pairs": len(routes),
        "dirs": len(routes) * 2,
        "packs": [{"pubkey": p.get("pubkey"), "n": len(p.get("addresses") or [])} for p in packs],
        "RACE_READY": n_ready,
        "RACE_READY_DIRS": n_ready * 2,
        "pct": (n_ready / len(routes) * 100.0) if routes else 0.0,
        "native_after": e.native_balance(wallet) / 1e9,
        "routes": ready_rows,
    }
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(rep, indent=2) + "\n", encoding="utf-8")
    print(
        f"ALT_PLANE done ready={n_ready}/{len(routes)} ({rep['pct']:.1f}%) "
        f"dirs={n_ready*2} native={rep['native_after']:.6f}",
        flush=True,
    )
    return rep


def main() -> int:
    live.load_dotenv()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "sync"
    if cmd == "register":
        register()
        return 0
    if cmd == "sync":
        sync()
        return 0
    print("usage: alt_plane.py register|sync")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
