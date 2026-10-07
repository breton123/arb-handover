#!/usr/bin/env python3
"""ARBHOPS0 production plane. New program id. Does not touch OUR_EXEC or oneshot ALT.

Stages: inventory | deploy | sim7 | size | publish | hurdle | audit | all
No funded send. FUNDED stays 0.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, "/home/louis/arb-cap")
sys.path.insert(0, "/home/louis/arb-exec/scripts")
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
import exec_live002b as x  # noqa: E402
import hops_vector as hv  # noqa: E402

from solders.compute_budget import set_compute_unit_limit, set_compute_unit_price
from solders.hash import Hash
from solders.instruction import AccountMeta, Instruction
from solders.keypair import Keypair
from solders.message import MessageV0
from solders.null_signer import NullSigner
from solders.pubkey import Pubkey
from solders.transaction import VersionedTransaction

try:
    from solders.address_lookup_table_account import AddressLookupTableAccount
except ImportError:  # pragma: no cover
    AddressLookupTableAccount = None  # type: ignore

DLMM = x.DLMM
PUMP = x.PUMP
SOL = x.SOL
TOKENKEG = x.TOKENKEG
TOKEN2022 = x.TOKEN2022
SYSTEM = x.SYSTEM
ATA_PROG = x.ATA_PROG
MEMO = x.MEMO
ALT_PROG = x.ALT_PROG
FEE_PROG = x.FEE_PROG
OUR_EXEC = "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K"
WALLET = "HHNzjTABPAbPY5uEXU7NTvfmcD9RYTdXtLntyKAtpXLX"
V0_MAX = 1232
ALT_MAX = 256
SWQOS = 150_000
SAFETY = 50_000
SIG = 5_000
CU_PRICE = 1_000_000
MIN_PROFIT = 1_000_000_000
TINY_IN = 10_000
SEQS = (
    "dlmm-dlmm",
    "dlmm-pump",
    "pump-dlmm",
    "pump-pump",
    "dlmm-dlmm-dlmm",
    "dlmm-dlmm-pump",
    "pump-dlmm-dlmm",
)

FAM6 = Path("/home/louis/arb-cap/fam6")
DUMP = FAM6 / "ROUTES.json"
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
PLANE = FAM6 / "hops_plane.json"
SIM = FAM6 / "SIM.json"
SIZE = FAM6 / "SIZE.json"
AUDIT = FAM6 / "AUDIT.json"
HURDLE_JSON = FAM6 / "HURDLE.json"
HURDLE_H = Path("/home/louis/arb-core/include/hops_hurdle.h")
DEPLOY = Path("/home/louis/arb-exec/.deploy")
HOPS_KP = DEPLOY / "hops-program.json"
HOPS_ID = DEPLOY / "hops-program-id.txt"
HOPS_SO = Path("/home/louis/arb-exec-live/program_hops/target/deploy/hops.so")
HOPS_ALTS = DEPLOY / "hops_alts.json"
ONESHOT_PLANE = DEPLOY / "alt_plane.json"
CACHE = FAM6 / "acc_cache.json"


def load_env() -> None:
    live.load_dotenv()


def payer() -> Keypair:
    return x.payer_kp()


def hops_pid() -> str:
    if HOPS_ID.exists():
        return HOPS_ID.read_text(encoding="utf-8").strip()
    if HOPS_KP.exists():
        raw = json.loads(HOPS_KP.read_text(encoding="utf-8"))
        return d._pk(bytes(raw[32:64]))
    raise SystemExit("no hops program id yet")


def native() -> int:
    b = d.rpc("getBalance", [WALLET])
    return b["value"] if isinstance(b, dict) else int(b)


def rent_exempt(n: int) -> int:
    return int(d.rpc("getMinimumBalanceForRentExemption", [n]))


def load_json(path: Path, default):
    if not path.exists():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def save_json(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")


def routes_pools():
    routes = load_json(DUMP, {})
    univ = load_json(UNIV, {})
    return routes.get("routes") or [], univ.get("pools") or []


_MINT_TOK: dict[str, str] = {SOL: TOKENKEG}


def mint_tok(mint: str) -> str:
    if mint in _MINT_TOK:
        return _MINT_TOK[mint]
    info = d.get_multiple([mint])[0]
    if not info:
        raise RuntimeError(f"mint missing {mint[:8]}")
    owner = info.get("owner")
    if owner not in (TOKENKEG, TOKEN2022):
        raise RuntimeError(f"mint owner {owner}")
    _MINT_TOK[mint] = owner
    return owner


def cache_get() -> dict:
    return load_json(CACHE, {})


def cache_put(obj: dict) -> None:
    save_json(CACHE, obj)


def _pump_meta(pair: str) -> dict:
    cache = cache_get()
    hit = cache.get("pump_meta", {}).get(pair)
    if hit and hit.get("coin_creator") and hit.get("base"):
        return hit
    pacc = d.get_multiple([pair])[0]
    if not pacc:
        raise RuntimeError(f"VECTOR_FAIL role=pool reason=MISSING pool={pair}")
    meta = hv.parse_pump_fields(pacc["data"])
    cache.setdefault("pump_meta", {})[pair] = meta
    cache_put(cache)
    return meta


def _dlmm_meta(pair: str) -> dict:
    # Never cache active_id / bins. A bin array is pool+index, not a reusable pubkey.
    sn = d.snapshot_pool(pair, [pair], None)
    if not sn or not sn.get("lb"):
        raise RuntimeError(f"VECTOR_FAIL role=lb_pair reason=SNAPSHOT pool={pair}")
    lb = sn["lb"]
    return {
        "mint_x": d._pk(lb["token_x"]),
        "mint_y": d._pk(lb["token_y"]),
        "vault_x": d._pk(lb["vault_x"]),
        "vault_y": d._pk(lb["vault_y"]),
        "active_id": lb["active_id"],
    }


def resolve_dlmm(pair: str) -> list[str]:
    return hv.derive_dlmm_hop(pair, _dlmm_meta(pair)).flatten()


def resolve_pump(pair: str, buy: bool, wallet: str, user_base: str, user_quote: str) -> list[str]:
    return hv.derive_pump_hop(
        pair, buy, wallet, user_base, user_quote, _pump_meta(pair),
    ).flatten(wallet)


def route_mints(pools: list, r: dict) -> list[str]:
    seq = r["seq"].split("-")
    n = int(r["n_hop"])
    mints = [SOL]
    idxs = [r.get("p0"), r.get("p1"), r.get("p2")][:n]
    for hi, proto in enumerate(seq):
        p = pools[int(idxs[hi])]
        mx = str(p.get("mx") or p.get("mint_x") or "")
        my = str(p.get("my") or p.get("mint_y") or "")
        if len(mx) < 32 or len(my) < 32:
            raise RuntimeError(
                f"VECTOR_FAIL route={r.get('id')} hop={hi} role=pool_mints "
                f"expected_mint=base58 got_mint=empty pool={p.get('pubkey')}"
            )
        incoming = mints[-1]
        outgoing = my if incoming == mx else mx
        mints.append(outgoing)
    return mints


def ata_of(wallet: str, mint: str) -> str:
    return x.ata(wallet, mint, mint_tok(mint))


def build_route_accounts(wallet: str, pools: list, r: dict) -> tuple[list[str], bytes, list]:
    seq = r["seq"].split("-")
    n = int(r["n_hop"])
    mints = route_mints(pools, r)
    users = [ata_of(wallet, mints[0])]
    for m in mints[1:]:
        pk = ata_of(wallet, m)
        if pk not in users:
            users.append(pk)
    while len(users) < 3:
        users.append(users[0])
    event_dlmm = d._pk(d.find_pda([b"__event_authority"], d.b58decode(DLMM)))
    shared = [
        wallet,
        users[0],
        users[1],
        users[2],
        TOKENKEG,
        TOKEN2022,
        MEMO,
        DLMM,
        event_dlmm,
    ]
    hops = []
    hops_sem = []
    ix_hops = []
    idxs = [r.get("p0"), r.get("p1"), r.get("p2")][:n]
    for hi, proto in enumerate(seq):
        p = pools[int(idxs[hi])]
        pair = p.get("pubkey") or p.get("pk")
        incoming = mints[hi]
        outgoing = mints[hi + 1]
        ina = 0 if incoming == SOL else (1 if incoming == mints[1] else 2)
        outa = 0 if outgoing == SOL else (1 if outgoing == mints[1] else 2)
        buy = proto == "pump" and incoming == SOL
        nacc = 10 if proto == "dlmm" else (26 if buy else 24)
        if proto == "dlmm":
            sem = hv.derive_dlmm_hop(pair, _dlmm_meta(pair))
            accs = sem.flatten()
        else:
            token = incoming if incoming != SOL else outgoing
            user_base = ata_of(wallet, token)
            user_quote = users[0]
            sem = hv.derive_pump_hop(
                pair, buy, wallet, user_base, user_quote, _pump_meta(pair),
            )
            accs = sem.flatten(wallet)
        if len(accs) != nacc:
            raise RuntimeError(f"{r['seq']} hop{hi} want {nacc} got {len(accs)}")
        hops.extend(accs)
        hops_sem.append(sem)
        ix_hops.append((1 if proto == "dlmm" else 2, ina, outa, nacc))
    keys = shared + hops
    buf = bytearray(40)
    buf[:8] = b"ARBHOPS0"
    buf[8] = n
    struct.pack_into("<QQ", buf, 9, TINY_IN, MIN_PROFIT)
    for i, (proto, ina, outa, nacc) in enumerate(ix_hops):
        buf[25 + i * 3] = proto
        buf[26 + i * 3] = ina
        buf[27 + i * 3] = outa
        buf[34 + i] = nacc
    return keys, bytes(buf), hops_sem


PROGRAMS = {
    TOKENKEG, TOKEN2022, SYSTEM, ATA_PROG, MEMO, DLMM, PUMP, FEE_PROG,
    "ComputeBudget111111111111111111111111111111",
    "11111111111111111111111111111111",
}

BPF_LOADERS = {
    "BPFLoaderUpgradeab1e11111111111111111111111",
    "BPFLoader21111111111111111111111111111111111",
}

VECTOR = FAM6 / "VECTOR.json"
PROVEN = ("dlmm-dlmm", "dlmm-pump", "pump-dlmm")

SHARED_ROLES = (
    "authority", "user0", "user1", "user2",
    "tokenkeg", "token2022", "memo", "dlmm_prog", "dlmm_event",
)
DLMM_ROLES = (
    "lb_pair", "bitmap", "vault_x", "vault_y", "oracle",
    "host", "mint_x", "mint_y", "bin0", "bin1",
)
PUMP_ROLES = hv.PUMP_HEAD_ROLES + hv.PUMP_BUY_TAIL


def fetch_accs(pks: list[str]) -> dict:
    out: dict = {}
    uniq = [p for p in dict.fromkeys(pks) if p]
    for i in range(0, len(uniq), 80):
        chunk = uniq[i:i + 80]
        rows = d.get_multiple(chunk)
        for pk, row in zip(chunk, rows):
            out[pk] = row
    return out


def tok_mint_owner(info) -> tuple[str | None, str | None]:
    raw = (info or {}).get("data")
    if not raw or len(raw) < 64:
        return None, None
    return d._pk(raw[0:32]), d._pk(raw[32:64])


def vector_fail(route, hop, role, **kw) -> dict:
    rec = {"route": route, "hop": hop, "role": role}
    rec.update(kw)
    return rec


def validate_vector(wallet: str, pools: list, r: dict, keys: list[str],
                    hops_sem: list | None = None) -> list[dict]:
    """Fail locally with VECTOR_FAIL records. No simulation."""
    fails = []
    rid = r.get("id")
    seq = r["seq"].split("-")
    n = int(r["n_hop"])
    try:
        mints = route_mints(pools, r)
    except Exception as e:
        return [vector_fail(rid, 0, "mints", detail=str(e))]
    if len(keys) < 9:
        return [vector_fail(rid, 0, "shared", detail=f"n_acc={len(keys)}")]
    infos = fetch_accs(keys + mints)
    if keys[0] != wallet:
        fails.append(vector_fail(rid, 0, "authority", expected=wallet, got=keys[0]))
    if keys[4] != TOKENKEG:
        fails.append(vector_fail(rid, 0, "tokenkeg", expected=TOKENKEG, got=keys[4]))
    if keys[5] != TOKEN2022:
        fails.append(vector_fail(rid, 0, "token2022", expected=TOKEN2022, got=keys[5]))
    if keys[7] != DLMM:
        fails.append(vector_fail(rid, 0, "dlmm_prog", expected=DLMM, got=keys[7]))
    dlmm_info = infos.get(keys[7]) or {}
    if dlmm_info.get("owner") not in BPF_LOADERS:
        fails.append(vector_fail(
            rid, 0, "dlmm_prog", expected_program="BPFLoader",
            got_program=dlmm_info.get("owner"), got=keys[7],
        ))
    for ui, mint in enumerate(mints[:3]):
        ata = keys[1 + ui] if 1 + ui < 4 else None
        if not mint or not ata:
            continue
        want_tok = mint_tok(mint)
        info = infos.get(ata) or {}
        got_mint, got_own = tok_mint_owner(info)
        if info.get("owner") != want_tok:
            fails.append(vector_fail(
                rid, 0, f"user{ui}", expected_program=want_tok,
                got_program=info.get("owner"), expected_mint=mint, got=ata,
            ))
        if got_mint != mint:
            fails.append(vector_fail(
                rid, 0, f"user{ui}", expected_mint=mint, got_mint=got_mint, got=ata,
            ))
        if got_own != wallet:
            fails.append(vector_fail(
                rid, 0, f"user{ui}", expected_owner=wallet, got_owner=got_own, got=ata,
            ))
    off = 9
    idxs = [r.get("p0"), r.get("p1"), r.get("p2")][:n]
    for hi, proto in enumerate(seq):
        p = pools[int(idxs[hi])]
        pair = p.get("pubkey") or p.get("pk")
        mx = str(p.get("mx") or p.get("mint_x") or "")
        my = str(p.get("my") or p.get("mint_y") or "")
        incoming, outgoing = mints[hi], mints[hi + 1]
        buy = proto == "pump" and incoming == SOL
        nacc = 10 if proto == "dlmm" else (26 if buy else 24)
        hop = keys[off:off + nacc]
        if len(hop) != nacc:
            fails.append(vector_fail(rid, hi, "hop_len", expected=nacc, got=len(hop)))
            break
        if proto == "dlmm":
            own = (infos.get(hop[0]) or {}).get("owner")
            if hop[0] != pair or own != DLMM:
                fails.append(vector_fail(
                    rid, hi, "lb_pair", expected=pair, got=hop[0],
                    expected_program=DLMM, got_program=own,
                ))
            if hop[6] != mx or hop[7] != my:
                fails.append(vector_fail(
                    rid, hi, "dlmm_mints", expected_mint=f"{mx}/{my}",
                    got_mint=f"{hop[6]}/{hop[7]}",
                ))
            for role, idx, mint in (("vault_x", 2, mx), ("vault_y", 3, my)):
                gm, go = tok_mint_owner(infos.get(hop[idx]))
                tok = mint_tok(mint)
                if (infos.get(hop[idx]) or {}).get("owner") != tok or gm != mint:
                    fails.append(vector_fail(
                        rid, hi, role, expected_mint=mint, got_mint=gm,
                        expected_program=tok,
                        got_program=(infos.get(hop[idx]) or {}).get("owner"),
                    ))
        else:
            own = (infos.get(hop[0]) or {}).get("owner")
            if hop[0] != pair or own != PUMP:
                fails.append(vector_fail(
                    rid, hi, "pool", expected=pair, got=hop[0],
                    expected_program=PUMP, got_program=own,
                ))
            if hop[16] != PUMP:
                fails.append(vector_fail(
                    rid, hi, "pump_prog", expected_program=PUMP, got_program=hop[16],
                ))
            if hop[3] != mx or hop[4] != my:
                fails.append(vector_fail(
                    rid, hi, "pump_mints", expected_mint=f"{mx}/{my}",
                    got_mint=f"{hop[3]}/{hop[4]}",
                ))
            token = incoming if incoming != SOL else outgoing
            for role, idx, mint, own_want in (
                ("user_base", 5, token, wallet),
                ("user_quote", 6, SOL, wallet),
            ):
                gm, go = tok_mint_owner(infos.get(hop[idx]))
                tok = mint_tok(mint)
                if (infos.get(hop[idx]) or {}).get("owner") != tok or gm != mint or go != own_want:
                    fails.append(vector_fail(
                        rid, hi, role, expected_mint=mint, got_mint=gm,
                        expected_program=tok,
                        got_program=(infos.get(hop[idx]) or {}).get("owner"),
                        expected_owner=own_want, got_owner=go,
                    ))
            if hop[11] != mint_tok(mx) or hop[12] != mint_tok(my):
                fails.append(vector_fail(
                    rid, hi, "pump_token_program",
                    expected_program=f"{mint_tok(mx)}/{mint_tok(my)}",
                    got_program=f"{hop[11]}/{hop[12]}",
                ))
            token = incoming if incoming != SOL else outgoing
            if hops_sem and hi < len(hops_sem):
                sem = hops_sem[hi]
            else:
                sem = hv.derive_pump_hop(
                    pair, buy, wallet, ata_of(wallet, token), ata_of(wallet, SOL),
                    _pump_meta(pair),
                )
            fails.extend(hv.validate_pump_keys(hop, sem, wallet, rid, hi, r["seq"]))
        if proto == "dlmm":
            if hops_sem and hi < len(hops_sem):
                sem = hops_sem[hi]
            else:
                sem = hv.derive_dlmm_hop(pair, _dlmm_meta(pair))
            fails.extend(hv.validate_dlmm_keys(hop, sem, rid, hi, r["seq"]))
        off += nacc
    return fails


def dump_vector_table(wallet: str, pools: list, r: dict, keys: list[str]) -> list[dict]:
    infos = fetch_accs(keys)
    rows = []
    for i, pk in enumerate(keys):
        info = infos.get(pk) or {}
        mint, owner = tok_mint_owner(info)
        role = SHARED_ROLES[i] if i < 9 else "hop"
        rows.append({
            "index": i,
            "pubkey": pk,
            "role": role,
            "writable": i == 0 or i in (1, 2, 3) or i >= 9,
            "signer": pk == wallet and i == 0,
            "owner": info.get("owner"),
            "mint": mint,
            "token_owner": owner,
            "alt_or_static": "static",
        })
    return rows


def print_vector_fails(fails: list[dict]) -> None:
    hv.print_vector_fails(fails)


def metas_for(keys: list[str], wallet: str) -> list[AccountMeta]:
    out = []
    for i, k in enumerate(keys):
        signer = k == wallet and i == 0
        writable = signer or i in (1, 2, 3) or i >= 9
        out.append(AccountMeta(Pubkey.from_string(k), signer, writable))
    return out


def compile_v0(payer_kp: Keypair, program: str, keys: list[str], data: bytes,
               alts: list, cu_limit: int) -> tuple[bytes, int]:
    ixs = [
        set_compute_unit_limit(cu_limit),
        set_compute_unit_price(1),
        Instruction(Pubkey.from_string(program), data, metas_for(keys, str(payer_kp.pubkey()))),
    ]
    bh = x.latest_blockhash()
    lut = []
    if alts and AddressLookupTableAccount is not None:
        for alt in alts:
            lut.append(AddressLookupTableAccount(
                Pubkey.from_string(alt["pubkey"]),
                [Pubkey.from_string(a) for a in alt["addresses"]],
            ))
    msg = MessageV0.try_compile(payer_kp.pubkey(), ixs, lut, bh)
    tx = VersionedTransaction(msg, [payer_kp])
    raw = bytes(tx)
    return raw, len(raw)


def compile_v0_size(payer_pk: str, program: str, keys: list[str], data: bytes,
                    alts: list, cu_limit: int) -> int:
    """Size only. Uses NullSigner so we can measure without the key."""
    ixs = [
        set_compute_unit_limit(cu_limit),
        set_compute_unit_price(1),
        Instruction(Pubkey.from_string(program), data, metas_for(keys, payer_pk)),
    ]
    bh = Hash.from_string("11111111111111111111111111111111")
    lut = []
    if alts and AddressLookupTableAccount is not None:
        for alt in alts:
            lut.append(AddressLookupTableAccount(
                Pubkey.from_string(alt.get("pubkey") or "11111111111111111111111111111111"),
                [Pubkey.from_string(a) for a in alt["addresses"]],
            ))
    msg = MessageV0.try_compile(Pubkey.from_string(payer_pk), ixs, lut, bh)
    tx = VersionedTransaction(msg, [NullSigner(Pubkey.from_string(payer_pk))])
    return len(bytes(tx))


def pick_seq_route(routes: list, seq: str) -> dict | None:
    for r in routes:
        if r.get("seq") == seq and r.get("exec"):
            return r
    return None


def cmd_inventory() -> int:
    load_env()
    sol = native()
    print(f"INVENTORY  wallet_sol={sol / 1e9:.9f}")
    print(f"  OUR_EXEC={OUR_EXEC} frozen")
    print(f"  oneshot_plane={ONESHOT_PLANE.exists()} (untouched)")
    print(f"  hops_so={HOPS_SO.exists()} bytes={HOPS_SO.stat().st_size if HOPS_SO.exists() else 0}")
    if HOPS_SO.exists():
        n = HOPS_SO.stat().st_size + 45
        try:
            r = rent_exempt(n)
        except Exception:
            r = n * 13920
        print(f"  programdata_rent={r / 1e9:.6f} peak_2x={2 * r / 1e9:.6f}")
        print(f"  deploy_feasible={sol > 2 * r + 20_000_000}")
    routes, pools = routes_pools()
    mints = set()
    for r in routes:
        if not r.get("exec"):
            continue
        for m in route_mints(pools, r)[1:]:
            if m and m != SOL:
                mints.add(m)
    missing = 0
    for m in sorted(mints):
        if not x.exists(ata_of(WALLET, m)):
            missing += 1
    print(f"  unique_non_sol_mints={len(mints)} ata_missing={missing}")
    print(f"  ata_rent_est={(missing * rent_exempt(165)) / 1e9:.6f}")
    buffers = []
    try:
        # solana-cli via RPC: program accounts of BPFLoaderUpgradeab owned by wallet is hard;
        # list known deploy leftovers.
        for p in DEPLOY.glob("*buffer*"):
            buffers.append(str(p))
        print(f"  local_buffer_files={buffers}")
    except Exception as e:
        print(f"  buffer_scan {e}")
    save_json(FAM6 / "INVENTORY.json", {
        "wallet_sol": sol / 1e9,
        "hops_so": HOPS_SO.exists(),
        "hops_so_bytes": HOPS_SO.stat().st_size if HOPS_SO.exists() else 0,
        "mints": len(mints),
        "ata_missing": missing,
        "our_exec": OUR_EXEC,
        "oneshot_untouched": True,
    })
    return 0


def cmd_deploy() -> int:
    load_env()
    if not HOPS_SO.exists():
        raise SystemExit("build hops.so first")
    if hops_already():
        print(f"DEPLOY  already {hops_pid()}")
        return 0
    sol = native()
    n = HOPS_SO.stat().st_size + 45
    need = rent_exempt(n)
    print(f"DEPLOY  so={HOPS_SO.stat().st_size} rent={need / 1e9:.6f} peak={2 * need / 1e9:.6f} have={sol / 1e9:.6f}")
    if sol < need + 15_000_000:
        save_json(FAM6 / "DEPLOY.json", {
            "ok": False,
            "reason": "FAIL_RENT",
            "have": sol / 1e9,
            "need_programdata": need / 1e9,
            "need_peak_2x": 2 * need / 1e9,
        })
        print("FAIL_RENT  cannot deploy ARBHOPS0 from this wallet")
        return 2
    DEPLOY.mkdir(parents=True, exist_ok=True)
    if not HOPS_KP.exists():
        os.system(
            f'export PATH="$HOME/.cargo/bin:$HOME/.local/share/solana/install/active_release/bin:$PATH"; '
            f'solana-keygen new -o {HOPS_KP} --no-bip39-passphrase --force >/tmp/s007/hops-keygen.log 2>&1'
        )
    pid = hops_pid()
    HOPS_ID.write_text(pid + "\n", encoding="utf-8")
    rpc = d.rpc_url()
    url_file = Path("/tmp/s007/rpc.url")
    url_file.write_text(rpc, encoding="utf-8")
    os.chmod(url_file, 0o600)
    wallet_json = DEPLOY / "wallet.json"
    cmd = (
        'export PATH="$HOME/.local/share/solana/install/active_release/bin:$PATH"; '
        f'solana program deploy {HOPS_SO} --program-id {HOPS_KP} '
        f'--keypair {wallet_json} --url "$(cat {url_file})" --use-rpc --max-sign-attempts 20'
    )
    print("DEPLOY  invoking solana program deploy (new id, not OUR_EXEC)")
    rc = os.system(cmd)
    try:
        url_file.unlink()
    except OSError:
        pass
    ok = x.exists(pid)
    save_json(FAM6 / "DEPLOY.json", {
        "ok": bool(ok and rc == 0),
        "program": pid,
        "rc": rc,
        "our_exec_untouched": OUR_EXEC,
        "wallet_sol_after": native() / 1e9,
    })
    print(f"DEPLOY  rc={rc} exists={ok} id={pid}")
    return 0 if ok else 3


def hops_already() -> bool:
    try:
        pid = hops_pid()
    except SystemExit:
        return False
    return x.exists(pid)


def is_custom6(err) -> bool:
    if not isinstance(err, dict):
        return False
    ie = err.get("InstructionError")
    return isinstance(ie, list) and len(ie) >= 2 and ie[1] == {"Custom": 6}


def load_hops_alts() -> list:
    return load_json(HOPS_ALTS, {}).get("alts") or []


def ensure_alts_for_keys(kp: Keypair, keysets: list[list[str]]) -> list:
    have = load_hops_alts()
    if have:
        return have
    dyn = set()
    static = {str(kp.pubkey()), TOKENKEG, TOKEN2022, SYSTEM, ATA_PROG, MEMO, DLMM, PUMP, FEE_PROG}
    try:
        static.add(hops_pid())
    except SystemExit:
        pass
    for keys in keysets:
        for a in keys:
            if a not in static:
                dyn.add(a)
    addrs = sorted(dyn)
    tables = plan_alts(addrs)
    print(f"SIM7  publishing {len(tables)} ALT(s) covering {len(addrs)} dynamic keys")
    alts = []
    for t in tables:
        pk = hops_create_alt(kp, t)
        alts.append({"pubkey": pk, "addresses": t})
    save_json(HOPS_ALTS, {"alts": alts, "n": len(addrs), "purpose": "hops_plane"})
    return alts


def route_has_t22(pools: list, r: dict) -> bool:
    try:
        for m in route_mints(pools, r):
            if mint_tok(m) == TOKEN2022:
                return True
    except Exception:
        return True
    return False


def seq_candidates(routes: list, pools: list, seq: str, limit: int = 64) -> list:
    out = []
    for r in routes:
        if r.get("seq") != seq or not r.get("exec"):
            continue
        out.append(r)
        if len(out) >= limit:
            break
    return out


def cmd_sim7() -> int:
    load_env()
    pid = hops_pid()
    if not x.exists(pid):
        print("SIM7  no deployed hops program")
        save_json(SIM, {"ok": False, "reason": "NO_PROGRAM"})
        return 2
    kp = payer()
    wallet = str(kp.pubkey())
    routes, pools = routes_pools()
    x.ensure_ata(kp, wallet, SOL, TOKENKEG)
    wsol = x.ata(wallet, SOL, TOKENKEG)
    try:
        x.wrap_wsol(kp, wsol, 50_000_000)
    except Exception as e:
        print(f"SIM7  wrap_wsol {e}")
    resolved = []
    for seq in SEQS:
        last_exc = None
        hit = None
        for r in seq_candidates(routes, pools, seq):
            try:
                for m in route_mints(pools, r):
                    x.ensure_ata(kp, wallet, m, mint_tok(m))
                keys, data, hops_sem = build_route_accounts(wallet, pools, r)
                hit = (seq, r, keys, data, hops_sem)
                break
            except Exception as e:
                last_exc = e
                print(f"SIM7  {seq} resolve-skip id={r.get('id')} {e}")
        if hit is None:
            resolved.append({"seq": seq, "ok": False, "reason": f"resolve {last_exc}"})
        else:
            resolved.append(hit)
    keysets = [x[2] for x in resolved if isinstance(x, tuple)]
    alts = ensure_alts_for_keys(kp, keysets) if keysets else []
    rows = []
    for item in resolved:
        if isinstance(item, dict):
            rows.append(item)
            print(f"SIM7  {item['seq']} {item.get('reason')}")
            continue
        seq, r, keys, data, hops_sem = item
        try:
            fails = validate_vector(wallet, pools, r, keys, hops_sem)
            if fails:
                print_vector_fails(fails[:8])
                rows.append({
                    "seq": seq, "id": r["id"], "ok": False,
                    "reason": "VECTOR_FAIL", "fails": fails[:8],
                })
                continue
            raw, n = compile_v0(kp, pid, keys, data, [], 400_000)
            used_alt = False
            if n > V0_MAX:
                raw, n = compile_v0(kp, pid, keys, data, alts, 400_000)
                used_alt = True
            if n > V0_MAX:
                rows.append({
                    "seq": seq, "id": r["id"], "ok": False,
                    "reason": f"oversize {n}", "raw": n, "n_acc": len(keys),
                })
                print(f"SIM7  {seq} OVERSIZE raw={n} n_acc={len(keys)}")
                continue
            sim = hv.hops_simulate(raw)
            err = sim.get("err")
            custom6 = is_custom6(err)
            if not custom6:
                expl = hv.explain_sim_fail(seq, r.get("id"), hops_sem, wallet, sim)
                if expl:
                    print_vector_fails(expl)
            if custom6:
                hv.persist_golden(seq, r, wallet, keys, hops_sem, sim, used_alt, pid)
            rows.append({
                "seq": seq,
                "id": r["id"],
                "ok": bool(custom6),
                "err": err,
                "cu": sim.get("cu"),
                "raw": n,
                "n_acc": len(keys),
                "alts": len(alts),
            })
            print(f"SIM7  {seq} custom6={custom6} cu={sim.get('cu')} raw={n} alt={used_alt} err={err}")
        except Exception as e:
            rows.append({"seq": seq, "id": r.get("id"), "ok": False, "reason": str(e)[:240]})
            print(f"SIM7  {seq} EXC {e}")
    ok = all(r.get("ok") for r in rows)
    save_json(SIM, {
        "ok": ok,
        "program": pid,
        "rows": rows,
        "min_profit": MIN_PROFIT,
        "alts": [a["pubkey"] for a in alts],
    })
    return 0 if ok else 4


def unique_dynamic(wallet: str, routes: list, pools: list) -> list[str]:
    keys = set()
    for r in routes:
        if not r.get("exec"):
            continue
        try:
            accs, _, _ = build_route_accounts(wallet, pools, r)
        except Exception:
            continue
        for a in accs:
            if a not in (wallet, TOKENKEG, TOKEN2022, SYSTEM, ATA_PROG, MEMO, DLMM, PUMP, FEE_PROG):
                keys.add(a)
    return sorted(keys)


def plan_alts(addrs: list[str]) -> list[list[str]]:
    return [addrs[i:i + ALT_MAX] for i in range(0, max(len(addrs), 1), ALT_MAX)]


def cmd_size() -> int:
    load_env()
    wallet = WALLET
    try:
        pid = hops_pid()
    except SystemExit:
        pid = "11111111111111111111111111111112"
    routes, pools = routes_pools()
    # Resolve a sample of each seq first so cache warms; then all.
    addrs = []
    failed = 0
    sizes = []
    # Warm cache + collect addresses using already-resolved routes.
    dyn = set()
    for r in routes:
        if not r.get("exec"):
            continue
        try:
            keys, data, _ = build_route_accounts(wallet, pools, r)
        except Exception as e:
            failed += 1
            sizes.append({"id": r["id"], "seq": r["seq"], "ok": False, "reason": str(e)[:160]})
            continue
        for a in keys:
            if a != wallet:
                dyn.add(a)
        # size without ALT first, then with planned ALTs
        tables = plan_alts(sorted(dyn))  # growing; final pass below
        _ = tables
        try:
            n = compile_v0_size(wallet, pid, keys, data, [], 300_000)
        except Exception as e:
            failed += 1
            sizes.append({"id": r["id"], "seq": r["seq"], "ok": False, "reason": f"compile {e}"[:160]})
            continue
        sizes.append({"id": r["id"], "seq": r["seq"], "ok": True, "raw_noalt": n, "n_acc": len(keys)})
    addrs = sorted(dyn)
    tables = plan_alts(addrs)
    planned = [{"pubkey": f"alt{i:03}", "addresses": t} for i, t in enumerate(tables)]
    over = 0
    final = []
    for row, r in zip(sizes, [rr for rr in routes if rr.get("exec")]):
        if not row.get("ok"):
            final.append(row)
            continue
        try:
            keys, data, _ = build_route_accounts(wallet, pools, r)
            n = compile_v0_size(wallet, pid, keys, data, planned, 300_000)
            ok = n <= V0_MAX
            if not ok:
                over += 1
            final.append({
                "id": r["id"],
                "seq": r["seq"],
                "ok": ok,
                "raw": n,
                "raw_noalt": row.get("raw_noalt"),
                "n_acc": len(keys),
            })
        except Exception as e:
            over += 1
            final.append({"id": r["id"], "seq": r["seq"], "ok": False, "reason": str(e)[:160]})
    obj = {
        "n": len(final),
        "ok": sum(1 for s in final if s.get("ok")),
        "over_1232": over,
        "failed_resolve": failed,
        "unique_dynamic": len(addrs),
        "alt_tables_planned": len(tables),
        "v0_max": V0_MAX,
        "rows": final,
    }
    save_json(SIZE, obj)
    print(
        f"SIZE  n={obj['n']} ok={obj['ok']} over={over} fail={failed} "
        f"dyn={len(addrs)} alts={len(tables)}",
        flush=True,
    )
    return 0 if obj["ok"] == 1530 and over == 0 else 5


def hops_create_alt(kp: Keypair, table: list[str]) -> str:
    slot = int(d.rpc("getSlot", [{"commitment": "confirmed"}]))
    raw, bump = x.alt_pda(bytes(kp.pubkey()), slot)
    alt = d._pk(raw)
    data = struct.pack("<IQB", 0, slot, bump)
    ix = Instruction(
        Pubkey.from_string(ALT_PROG),
        data,
        [
            AccountMeta(Pubkey.from_string(alt), False, True),
            AccountMeta(kp.pubkey(), True, False),
            AccountMeta(kp.pubkey(), True, True),
            AccountMeta(Pubkey.from_string(SYSTEM), False, False),
        ],
    )
    sig = x.send_ok(kp, [set_compute_unit_price(1), ix])
    print(f"  ALT create {alt[:8]} {sig[:12]}")
    for off in range(0, len(table), 20):
        chunk = table[off:off + 20]
        payload = struct.pack("<IQ", 2, len(chunk)) + b"".join(d.b58decode(a) for a in chunk)
        eix = Instruction(
            Pubkey.from_string(ALT_PROG),
            payload,
            [
                AccountMeta(Pubkey.from_string(alt), False, True),
                AccountMeta(kp.pubkey(), True, False),
                AccountMeta(kp.pubkey(), True, True),
                AccountMeta(Pubkey.from_string(SYSTEM), False, False),
            ],
        )
        sig = x.send_ok(kp, [set_compute_unit_price(1), eix])
        print(f"  ALT extend {off}+{len(chunk)} {sig[:12]}")
    got = []
    for _ in range(24):
        got = x.alt_addresses(alt)
        if got == table:
            return alt
        time.sleep(0.5)
    raise RuntimeError(f"ALT mismatch have={len(got)} want={len(table)}")


def cmd_publish() -> int:
    load_env()
    kp = payer()
    wallet = str(kp.pubkey())
    routes, pools = routes_pools()
    plane = load_json(PLANE, {})
    # ATAs for unique mints. Stop before we strand deploy rent.
    mints = set()
    for r in routes:
        if r.get("exec"):
            mints.update(route_mints(pools, r))
    created = 0
    missing = 0
    reserve = 40_000_000
    for m in sorted(mints):
        tok = mint_tok(m)
        pk = x.ata(wallet, m, tok)
        if x.exists(pk):
            continue
        if native() < reserve + rent_exempt(165) + 5_000_000:
            missing += 1
            continue
        try:
            x.ensure_ata(kp, wallet, m, tok)
            created += 1
        except Exception as e:
            print(f"  ATA fail {m[:8]} {e}")
            missing += 1
    addrs = unique_dynamic(wallet, routes, pools)
    tables = plan_alts(addrs)
    published = load_json(HOPS_ALTS, {"alts": []})
    alts = published.get("alts") or []
    if not alts:
        rent_one = rent_exempt(56 + 32 * min(len(tables[0]) if tables else 1, ALT_MAX))
        if native() < rent_one * len(tables) + reserve:
            print(f"PUBLISH  FAIL_RENT alts={len(tables)} rent_one={rent_one / 1e9:.4f} have={native() / 1e9:.4f}")
        else:
            for t in tables:
                pk = hops_create_alt(kp, t)
                alts.append({"pubkey": pk, "addresses": t})
            save_json(HOPS_ALTS, {"alts": alts, "n": len(addrs)})
    ata_ready = 0
    race = 0
    size_rows = {s["id"]: s for s in load_json(SIZE, {}).get("rows") or [] if "id" in s}
    for r in plane.get("routes") or []:
        mints_r = r.get("mints") or []
        atas_ok = all(x.exists(ata_of(wallet, m)) for m in mints_r if m)
        sz = size_rows.get(r["id"], {})
        size_ok = bool(sz.get("ok")) and int(sz.get("raw") or 9999) <= V0_MAX
        if atas_ok:
            ata_ready += 1
        r["ata_ready"] = int(atas_ok)
        r["size_ok"] = int(size_ok)
        r["alt_ready"] = int(bool(alts))
        r["vector_ready"] = int(atas_ok and size_ok)
        # RACE_READY requires Custom(6). Publish never invents it.
        prev_race = int(r.get("race_ready") or 0)
        r["race_ready"] = int(prev_race and r["vector_ready"])
        if r["race_ready"]:
            race += 1
    vec_n = sum(1 for r in (plane.get("routes") or []) if r.get("vector_ready"))
    plane["alt_published"] = len(alts)
    plane["ata_created"] = created
    plane["ata_ready"] = ata_ready
    plane["vector_ready"] = vec_n
    plane["race_ready"] = race
    plane["unique_dynamic"] = len(addrs)
    plane["alt_tables_planned"] = len(tables)
    plane["program"] = hops_pid() if hops_already() else plane.get("program")
    save_json(PLANE, plane)
    print(
        f"PUBLISH  alts={len(alts)} ata_created={created} ata_ready={ata_ready} "
        f"RACE_READY={race} missing_ata={missing}",
        flush=True,
    )
    return 0


def write_hurdle_h(rows: list) -> None:
    lines = [
        "#ifndef ARB_CORE_HOPS_HURDLE_H",
        "#define ARB_CORE_HOPS_HURDLE_H",
        "",
        "#include <stdint.h>",
        "#include <string.h>",
        "",
        "#define HOPS_SWQOS_FEE   150000ull",
        "#define HOPS_SAFETY_FEE   50000ull",
        "#define HOPS_SIG_FEE       5000ull",
        f"#define HOPS_CU_PRICE   {CU_PRICE}ull",
        "",
        "static inline uint64_t hops_min_gross(uint64_t cu_limit)",
        "{",
        "    return HOPS_SIG_FEE + (cu_limit * HOPS_CU_PRICE) / 1000000ull",
        "        + HOPS_SWQOS_FEE + HOPS_SAFETY_FEE;",
        "}",
        "",
        "static inline uint64_t hops_hurdle_for_seq(const char *seq)",
        "{",
        "    if (seq == NULL) {",
        "        return hops_min_gross(200000ull);",
        "    }",
    ]
    by = {r["seq"]: r for r in rows if r.get("ok") and r.get("cu")}
    for seq in SEQS:
        r = by.get(seq)
        if r and r.get("cu"):
            lim = int(int(r["cu"]) * 12 / 10)
        else:
            lim = 200000
        lines.append(f'    if (strcmp(seq, "{seq}") == 0) {{')
        lines.append(f"        return hops_min_gross({lim}ull);")
        lines.append("    }")
    lines += [
        "    return hops_min_gross(200000ull);",
        "}",
        "",
        "#endif",
        "",
    ]
    HURDLE_H.write_text("\n".join(lines), encoding="utf-8")
    meta = []
    for seq in SEQS:
        r = by.get(seq) or {}
        cu = int(r["cu"]) if r.get("cu") else None
        lim = int(cu * 12 / 10) if cu else 200000
        onchain = SIG + (lim * CU_PRICE) // 1_000_000
        min_gross = onchain + SWQOS + SAFETY
        meta.append({
            "seq": seq,
            "cu": cu,
            "cu_limit": lim,
            "cu_price": CU_PRICE,
            "onchain_fee": onchain,
            "swqos_fee": SWQOS,
            "safety": SAFETY,
            "min_gross": min_gross,
            "custom6": bool(r.get("ok")),
        })
    save_json(HURDLE_JSON, {"rows": meta})
    print("HURDLE  wrote hops_hurdle.h")
    for m in meta:
        print(f"  {m['seq']:16} cu={m['cu']} lim={m['cu_limit']} min_gross={m['min_gross']}")


def cmd_recover() -> int:
    """Restore Custom(6) on the three previously proven sequences.

    Does not touch ARBHOPS0 or the route compiler. Rebuilds json mints,
    validates the vector, then simulates.
    """
    load_env()
    sys.path.insert(0, "/home/louis/arb-cap/univ")
    import rewrite_univ_json
    argv = sys.argv
    sys.argv = ["rewrite_univ_json.py"]
    try:
        rewrite_univ_json.main()
    finally:
        sys.argv = argv
    pid = hops_pid()
    if not x.exists(pid):
        print("RECOVER  no hops program")
        return 2
    kp = payer()
    wallet = str(kp.pubkey())
    routes, pools = routes_pools()
    missing = sum(1 for p in pools if not (p.get("mx") or p.get("mint_x")))
    if missing:
        print(f"RECOVER  FAIL liveuniv.json missing_mint={missing}")
        return 3
    x.ensure_ata(kp, wallet, SOL, TOKENKEG)
    try:
        x.wrap_wsol(kp, x.ata(wallet, SOL, TOKENKEG), 50_000_000)
    except Exception as e:
        print(f"RECOVER  wrap_wsol {e}")
    rows = []
    dumped = False
    for seq in PROVEN:
        last = None
        won = None
        for r in seq_candidates(routes, pools, seq, limit=24):
            try:
                for m in route_mints(pools, r):
                    x.ensure_ata(kp, wallet, m, mint_tok(m))
                keys, data, hops_sem = build_route_accounts(wallet, pools, r)
            except Exception as e:
                print(f"RECOVER  {seq} resolve-skip id={r.get('id')} {e}")
                last = {"seq": seq, "id": r.get("id"), "ok": False, "reason": str(e)[:200]}
                continue
            table = dump_vector_table(wallet, pools, r, keys)
            fails = validate_vector(wallet, pools, r, keys, hops_sem)
            fails.extend(hv.check_golden(seq, r.get("id"), keys))
            if seq == "dlmm-pump" and not dumped:
                save_json(VECTOR, {
                    "seq": seq, "id": r["id"], "n_acc": len(keys),
                    "fails": fails, "accounts": table,
                })
                dumped = True
                print(f"RECOVER  dumped VECTOR.json id={r['id']} n_acc={len(keys)} fails={len(fails)}")
            if fails:
                print(f"RECOVER  {seq} id={r.get('id')} VECTOR_FAIL n={len(fails)}")
                print_vector_fails(fails[:6])
                last = {"seq": seq, "id": r["id"], "ok": False, "reason": "VECTOR_FAIL", "fails": fails[:8]}
                continue
            raw, n = compile_v0(kp, pid, keys, data, [], 400_000)
            used_alt = False
            if n > V0_MAX:
                alts = ensure_alts_for_keys(kp, [keys])
                raw, n = compile_v0(kp, pid, keys, data, alts, 400_000)
                used_alt = True
            if n > V0_MAX:
                last = {"seq": seq, "id": r["id"], "ok": False, "reason": f"oversize {n}"}
                print(f"RECOVER  {seq} OVERSIZE raw={n}")
                continue
            sim = hv.hops_simulate(raw)
            err = sim.get("err")
            custom6 = is_custom6(err)
            if not custom6:
                expl = hv.explain_sim_fail(seq, r.get("id"), hops_sem, wallet, sim)
                if expl:
                    print_vector_fails(expl)
            else:
                hv.persist_golden(seq, r, wallet, keys, hops_sem, sim, used_alt, pid)
            last = {
                "seq": seq, "id": r["id"], "ok": bool(custom6),
                "err": err, "cu": sim.get("cu"), "raw": n,
                "alt": used_alt, "vector_ready": 1,
                "race_ready": int(custom6),
            }
            print(
                f"RECOVER  {seq} id={r['id']} custom6={custom6} cu={sim.get('cu')} "
                f"raw={n} alt={used_alt} err={err}",
                flush=True,
            )
            if custom6:
                won = last
                break
        rows.append(won or last or {"seq": seq, "ok": False, "reason": "no_candidate"})
    proven_ok = all(r.get("ok") for r in rows)
    print(
        f"RECOVER  proven {sum(1 for r in rows if r.get('ok'))}/3 "
        f"{'PASS' if proven_ok else 'FAIL'}",
        flush=True,
    )
    if proven_ok:
        rest = [s for s in SEQS if s not in PROVEN]
        for seq in rest:
            last = None
            won = None
            for r in seq_candidates(routes, pools, seq, limit=24):
                try:
                    for m in route_mints(pools, r):
                        x.ensure_ata(kp, wallet, m, mint_tok(m))
                    keys, data, hops_sem = build_route_accounts(wallet, pools, r)
                except Exception as e:
                    last = {"seq": seq, "id": r.get("id"), "ok": False, "reason": str(e)[:200]}
                    print(f"RECOVER  {seq} resolve-skip id={r.get('id')} {e}")
                    continue
                fails = validate_vector(wallet, pools, r, keys, hops_sem)
                fails.extend(hv.check_golden(seq, r.get("id"), keys))
                if fails:
                    print(f"RECOVER  {seq} id={r.get('id')} VECTOR_FAIL n={len(fails)}")
                    print_vector_fails(fails[:4])
                    last = {"seq": seq, "id": r["id"], "ok": False, "reason": "VECTOR_FAIL"}
                    continue
                raw, n = compile_v0(kp, pid, keys, data, [], 400_000)
                used_alt = False
                if n > V0_MAX:
                    alts = ensure_alts_for_keys(kp, [keys])
                    raw, n = compile_v0(kp, pid, keys, data, alts, 400_000)
                    used_alt = True
                if n > V0_MAX:
                    last = {"seq": seq, "id": r["id"], "ok": False, "reason": f"oversize {n}"}
                    continue
                sim = hv.hops_simulate(raw)
                err = sim.get("err")
                custom6 = is_custom6(err)
                if not custom6:
                    expl = hv.explain_sim_fail(seq, r.get("id"), hops_sem, wallet, sim)
                    if expl:
                        print_vector_fails(expl)
                else:
                    hv.persist_golden(seq, r, wallet, keys, hops_sem, sim, used_alt, pid)
                last = {
                    "seq": seq, "id": r["id"], "ok": bool(custom6),
                    "err": err, "cu": sim.get("cu"), "raw": n,
                    "alt": used_alt, "vector_ready": 1,
                    "race_ready": int(custom6),
                }
                print(
                    f"RECOVER  {seq} id={r['id']} custom6={custom6} cu={sim.get('cu')} "
                    f"raw={n} alt={used_alt} err={err}",
                    flush=True,
                )
                if custom6:
                    won = last
                    break
            rows.append(won or last or {"seq": seq, "ok": False, "reason": "no_candidate"})
    all_ok = all(r.get("ok") for r in rows)
    save_json(SIM, {
        "ok": all_ok,
        "mode": "recover",
        "program": pid,
        "rows": rows,
        "min_profit": MIN_PROFIT,
    })
    print(
        f"RECOVER  sequences {sum(1 for r in rows if r.get('ok'))}/{len(rows)} "
        f"{'PASS' if all_ok else 'FAIL'}",
        flush=True,
    )
    write_hurdle_h(rows)
    return 0 if proven_ok else 4


def cmd_hurdle() -> int:
    sim = load_json(SIM, {})
    write_hurdle_h(sim.get("rows") or [])
    return 0


def cmd_audit() -> int:
    routes, _pools = routes_pools()
    plane = load_json(PLANE, {})
    size = load_json(SIZE, {})
    sim = load_json(SIM, {})
    n = len([r for r in routes if r.get("exec")])
    tmpl = len(plane.get("routes") or [])
    ata = int(plane.get("ata_ready") or 0)
    vec = int(plane.get("vector_ready") or 0)
    race = int(plane.get("race_ready") or 0)
    exec_missing = len([r for r in routes if not r.get("exec")])
    fam255 = len([r for r in routes if r.get("fam") == 255])
    proven = [s for s in (sim.get("rows") or []) if s.get("seq") in PROVEN and s.get("ok")]
    obj = {
        "compiled": n,
        "searchable": n,
        "state": n,
        "executable": n,
        "templates": tmpl,
        "VECTOR_READY": vec,
        "RACE_READY": race,
        "alt_ata_ready": ata,
        "EXEC_MISSING": exec_missing,
        "family255": fam255,
        "size_ok": size.get("ok"),
        "sim_ok": sim.get("ok"),
        "proven_custom6": [s.get("seq") for s in proven],
        "complete": n > 0 and tmpl == n and vec == n and race == n
        and exec_missing == 0 and fam255 == 0,
    }
    save_json(AUDIT, obj)
    print(
        f"AUDIT  compiled {n} / searchable {n} / executable {n} / "
        f"VECTOR_READY {vec} / RACE_READY {race} / "
        f"EXEC_MISSING {exec_missing} / family255 {fam255} "
        f"proven={obj['proven_custom6']} complete={obj['complete']}"
    )
    return 0 if obj["complete"] else 6


def cmd_incr() -> int:
    """New compiled routes provision while existing RACE_READY stay usable."""
    load_env()
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import hops_plane
    hops_plane.main()
    rc = cmd_publish()
    cmd_audit()
    return rc


def cmd_all() -> int:
    cmd_inventory()
    if not HOPS_SO.exists():
        print("ALL  hops.so missing — run build_hops.sh")
        return 1
    rc = cmd_deploy()
    if rc != 0:
        print("ALL  deploy failed; still running size + hurdle placeholders + audit")
        cmd_size()
        cmd_hurdle()
        cmd_audit()
        return rc
    cmd_sim7()
    cmd_hurdle()
    cmd_size()
    cmd_publish()
    return cmd_audit()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="?", default="all",
                    choices=["inventory", "deploy", "sim7", "size", "publish",
                             "hurdle", "audit", "incr", "recover", "all"])
    args = ap.parse_args()
    return {
        "inventory": cmd_inventory,
        "deploy": cmd_deploy,
        "sim7": cmd_sim7,
        "size": cmd_size,
        "publish": cmd_publish,
        "hurdle": cmd_hurdle,
        "audit": cmd_audit,
        "incr": cmd_incr,
        "recover": cmd_recover,
        "all": cmd_all,
    }[args.cmd]()


if __name__ == "__main__":
    raise SystemExit(main())
