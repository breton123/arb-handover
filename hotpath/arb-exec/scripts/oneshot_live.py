#!/usr/bin/env python3
"""ONESHOT — one fresh FRAMED opp, patch resident template, racer send.

Never retries. No fee ladder. No nonce spray. Leaves frozen kernels alone.
Race path: no resolve_pair, no RPC, no ALT discovery.
"""
from __future__ import annotations

import json
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-cap"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
import exec_live002b as e  # noqa: E402

from solders.signature import Signature

AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
OUT = Path("/home/louis/arb-cap/oneshot")
ARMED = OUT / "ARMED"
DISARMED = OUT / "DISARMED"
RESULT = OUT / "RESULT.json"
SWQOS_SOCK = Path("/home/louis/arb-cap/oneshot/swqos.sock")
PLANE = Path("/home/louis/arb-exec/.deploy/alt_plane.json")
PLANE_REPORT = Path("/home/louis/arb-cap/oneshot/ALT_PLANE.json")

MAX_IN = 50_000_000
MIN_IN = 10_000_000
CU_LIMIT = 400_000
CU_PRICE = 800_000
# Four landed Custom(6)s billed fee=325000 = CU_LIMIT * CU_PRICE / 1e6 + 5000.
# SWQOS prepaid 150000 per send (account dropped 150k each attempt).
ONCHAIN_FEE = CU_LIMIT * CU_PRICE // 1_000_000 + 5_000
SWQOS_UNIT = 150_000
SAFETY = 50_000
HURDLE = ONCHAIN_FEE + SWQOS_UNIT + SAFETY  # 525000
MIN_PROFIT = HURDLE
WAIT_S = 5400
S007_READY = Path("/home/louis/captures/state007/READY")
HEARTBEAT_S = 30
FUNDED = os.environ.get("FUNDED", "0") == "1"
COOL = OUT / "COOLDOWN.json"
MISSING_BLOCK = 3


def disarm(why: str, extra: dict | None = None) -> None:
    payload = {"why": why, "ts": time.time(), **(extra or {})}
    OUT.mkdir(parents=True, exist_ok=True)
    if ARMED.exists():
        ARMED.unlink()
    DISARMED.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    RESULT.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"DISARMED  {why}", flush=True)


def univ_pools() -> list[dict]:
    return (json.loads(UNIV.read_text(encoding="utf-8")).get("pools") or [])


def pool_at(pools: list[dict], idx: int) -> dict | None:
    if 0 <= idx < len(pools):
        return pools[idx]
    return None


def partner(pools: list[dict], meta: dict, want: str) -> dict | None:
    tok = meta.get("token")
    if not tok:
        return None
    for p in pools:
        proto = str(p.get("proto") or p.get("kind") or "")
        if proto == want and p.get("token") == tok:
            return p
    return None


def load_cool() -> dict:
    if not COOL.exists():
        return {"routes": {}}
    try:
        return json.loads(COOL.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"routes": {}}


def save_cool(obj: dict) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    COOL.write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")


def route_blocked(pool: str) -> bool:
    routes = load_cool().get("routes") or {}
    if (routes.get(pool) or {}).get("SEND_BLOCKED"):
        return True
    pref = pool[:8]
    for k, row in routes.items():
        if row.get("SEND_BLOCKED") and (pool.startswith(k) or k.startswith(pref)):
            return True
    return False


def note_trigger_missing(pool: str) -> dict:
    obj = load_cool()
    routes = obj.setdefault("routes", {})
    row = routes.setdefault(pool, {"trigger_missing": 0, "SEND_BLOCKED": False})
    row["trigger_missing"] = int(row.get("trigger_missing") or 0) + 1
    if row["trigger_missing"] >= MISSING_BLOCK:
        row["SEND_BLOCKED"] = True
        row["why"] = "TRIGGER_MISSING x3"
    routes[pool] = row
    save_cool(obj)
    return row


def raw_ns() -> int:
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)


_bh_lock = threading.Lock()
_bh = b""
_plane: dict[str, dict] = {}
_hash_stop = False


def load_race_plane() -> dict[str, dict]:
    by: dict[str, dict] = {}
    srcs = []
    if PLANE.exists():
        srcs.append(json.loads(PLANE.read_text(encoding="utf-8")).get("routes") or [])
    if PLANE_REPORT.exists():
        srcs.append(json.loads(PLANE_REPORT.read_text(encoding="utf-8")).get("routes") or [])
    for routes in srcs:
        for r in routes:
            if not r.get("RACE_READY") or not r.get("tmpl0") or not r.get("tmpl1"):
                continue
            rec = {
                "dlmm": r["dlmm"],
                "pump": r["pump"],
                "token": r.get("token"),
                "alt": r.get("alt"),
                "tmpl0": bytes.fromhex(r["tmpl0"]),
                "tmpl1": bytes.fromhex(r["tmpl1"]),
            }
            by[r["dlmm"]] = rec
            by[r["pump"]] = rec
    return by


def hash_loop() -> None:
    global _bh
    while not _hash_stop:
        try:
            raw = d.rpc(
                "getLatestBlockhash",
                [{"commitment": "processed"}],
                retries=1,
                backoff=2.0,
            )
            bh = d.b58decode(raw["value"]["blockhash"])
            if len(bh) == 32:
                with _bh_lock:
                    _bh = bh
        except Exception:
            time.sleep(2.0)
            continue
        time.sleep(2.0)


def resident_bh() -> bytes:
    with _bh_lock:
        return _bh


def racer_send(tx: bytes) -> tuple[int, int, int]:
    if len(tx) < 64 or len(tx) > 1232:
        raise RuntimeError(f"bad tx len {len(tx)}")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect(str(SWQOS_SOCK))
    s.sendall(struct.pack("<H", len(tx)) + tx)
    rep = b""
    while len(rep) < 20:
        chunk = s.recv(20 - len(rep))
        if not chunk:
            break
        rep += chunk
    s.close()
    if len(rep) != 20:
        raise RuntimeError("racer short reply")
    rc = struct.unpack_from("<i", rep, 0)[0]
    t0 = struct.unpack_from("<Q", rep, 4)[0]
    t1 = struct.unpack_from("<Q", rep, 12)[0]
    return rc, t0, t1


def framed_ready(rec: dict) -> bool:
    """#6 contract: FRAMED ∧ mut_authoritative ∧ STATE007_READY ∧ ALT RACE_READY."""
    fr = rec.get("frame") or {}
    pool = str(rec.get("pool") or "")
    return (
        fr.get("class") == "framed"
        and rec.get("race_ready") == 1
        and S007_READY.exists()
        and bool(pool)
        and pool in _plane
    )


def size_gate(rec: dict) -> tuple[int, int] | None:
    arb = rec.get("arb") or {}
    if not framed_ready(rec):
        return None
    direction = arb.get("direction")
    if direction is None or int(direction) not in (0, 1):
        return None
    if route_blocked(str(rec.get("pool") or "")):
        return None
    sq = rec.get("send_quote") or {}
    # Frozen cycle_quote at the actual cap. Never scale optimal gross.
    if not sq.get("cap_ok"):
        return None
    gross = int(sq.get("cap_gross") or 0)
    send = MAX_IN
    if send < MIN_IN or gross <= HURDLE:
        return None
    return send, gross


def send_journal(rec: dict, gate: tuple[int, int] | None) -> dict:
    arb = rec.get("arb") or {}
    sq = rec.get("send_quote") or {}
    j = {
        "optimal": {
            "amount": int(arb.get("amount_in") or 0),
            "gross": int(arb.get("gross") or 0),
            "final": int(arb.get("final") or 0),
        },
        "actual": {
            "amount": MAX_IN,
            "final": int(sq.get("cap_final") or 0),
            "gross": int(sq.get("cap_gross") or 0),
            "quoted": bool(sq.get("cap_ok")),
        },
        "cost_hurdle": {
            "SWQOS": SWQOS_UNIT,
            "base": 5_000,
            "priority": ONCHAIN_FEE - 5_000,
            "onchain_fee": ONCHAIN_FEE,
            "margin": SAFETY,
            "required": HURDLE,
            "note": "priority is CU_LIMIT*CU_PRICE/1e6; four attempts billed 325000",
        },
        "decision": "DROP" if gate is None else "SEND",
        "frame": rec.get("frame") or {},
        "race_ready": rec.get("race_ready"),
        "prereq": {
            "CORE010_FRAMED": (rec.get("frame") or {}).get("class") == "framed",
            "RACE_READY": rec.get("race_ready") == 1,
            "SYNCED": True,
            "exact_size_gt_hurdle": bool(gate),
        },
    }
    fr = rec.get("frame") or {}
    print(
        f"  journal opt={j['optimal']['amount']} gp={j['optimal']['gross']} "
        f"send={j['actual']['amount']} send_gp={j['actual']['gross']} "
        f"hurdle={HURDLE} frame={fr.get('class')} delay_ns={fr.get('delay_ns')} "
        f"{j['decision']}",
        flush=True,
    )
    return j


def creator_from_pool(raw: bytes, quote: str) -> tuple[str, str]:
    fb = e.pump_fallback()
    if len(raw) < 243:
        return fb["creator_auth"], fb["creator_ata"]
    coin = raw[211:243]
    if coin == bytes(32):
        return fb["creator_auth"], fb["creator_ata"]
    auth = d._pk(d.find_pda([b"creator_vault", coin], d.b58decode(e.PUMP)))
    return auth, e.ata(auth, quote, e.TOKENKEG)


def resolve_pair(wallet: str, pair_dlmm: str, pair_pump: str) -> dict:
    e.PAIR_DLMM = pair_dlmm
    e.PAIR_PUMP = pair_pump
    sn = d.snapshot_pool(pair_dlmm, [pair_dlmm], None)
    if not sn or not sn.get("lb"):
        raise RuntimeError("DLMM snapshot failed")
    lb = sn["lb"]
    mint_x = d._pk(lb["token_x"])
    mint_y = d._pk(lb["token_y"])
    if mint_y != e.SOL:
        raise RuntimeError(f"dlmm Y is not WSOL {mint_y[:8]}")
    active_arr = d.bin_array_index(lb["active_id"])
    bins = []
    for i in (active_arr, active_arr + 1, active_arr - 1, active_arr + 2, active_arr - 2):
        pk = d.bin_array_pda(pair_dlmm, i)
        if pk not in bins and e.exists(pk):
            bins.append(pk)
        if len(bins) == 2:
            break
    if len(bins) < 2:
        raise RuntimeError("need two live bin arrays")
    pacc = d.get_multiple([pair_pump])[0]
    p = live.parse_pump_pool(pacc["data"]) if pacc else None
    if not p:
        raise RuntimeError("pump parse failed")
    mint_info = d.get_multiple([mint_x])[0]
    base_tok = (mint_info or {}).get("owner") or e.TOKEN2022
    if base_tok not in (e.TOKENKEG, e.TOKEN2022):
        raise RuntimeError(f"bad base mint owner {base_tok}")
    pump = e.pump_fallback()
    c_auth, c_ata = creator_from_pool(pacc["data"], mint_y)
    pump["creator_auth"] = c_auth
    pump["creator_ata"] = c_ata
    fee_recipient = e.FEE_RECIPIENT
    fee_rec_quote = e.ata(fee_recipient, mint_y, e.TOKENKEG)
    if not e.exists(fee_rec_quote):
        for rec in e.FEE_RECIPIENTS:
            q = e.ata(rec, mint_y, e.TOKENKEG)
            if e.exists(q):
                fee_recipient, fee_rec_quote = rec, q
                break
        else:
            raise RuntimeError("no fee_recipient_quote ATA")
    return {
        "pair_dlmm": pair_dlmm,
        "pair_pump": pair_pump,
        "mint_x": mint_x,
        "mint_y": mint_y,
        "base_tok": base_tok,
        "user_quote": e.ata(wallet, e.SOL, e.TOKENKEG),
        "user_base": e.ata(wallet, mint_x, base_tok),
        "event_dlmm": d._pk(d.find_pda([b"__event_authority"], d.b58decode(e.DLMM))),
        "oracle": d._pk(d.find_pda([b"oracle", d.b58decode(pair_dlmm)], d.b58decode(e.DLMM))),
        "bins": bins,
        "vault_x": d._pk(lb["vault_x"]),
        "vault_y": d._pk(lb["vault_y"]),
        "vault_b": d._pk(p["vault_base"]),
        "vault_q": d._pk(p["vault_quote"]),
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
        "active_id": lb["active_id"],
    }


def follow_new(path: Path):
    with path.open("r", encoding="utf-8") as f:
        f.seek(0, os.SEEK_END)
        while True:
            line = f.readline()
            if line:
                yield line
            else:
                time.sleep(0.02)
                yield None


def _tx_slot(sig: str) -> dict:
    try:
        tx = d.rpc(
            "getTransaction",
            [sig, {"encoding": "json", "maxSupportedTransactionVersion": 0}],
        )
    except Exception:
        return {"sig": sig, "slot": None, "err": "rpc"}
    if not tx:
        return {"sig": sig, "slot": None, "err": "missing"}
    return {
        "sig": sig,
        "slot": tx.get("slot"),
        "err": (tx.get("meta") or {}).get("err"),
        "fee": (tx.get("meta") or {}).get("fee"),
    }


def classify_race(rec: dict, fire_out: dict, why: str, n_info: dict) -> dict:
    """Post-send ordering. Not on the race path."""
    n_sig = n_info.get("n_sig")
    ours = fire_out.get("sig")
    n_row = _tx_slot(n_sig) if n_sig else {"slot": None}
    o_row = _tx_slot(ours) if ours else {"slot": None}
    n_slot = n_row.get("slot")
    o_slot = o_row.get("slot")
    others = []
    dlmm = fire_out.get("dlmm")
    if dlmm and n_slot:
        try:
            hist = d.rpc("getSignaturesForAddress", [dlmm, {"limit": 20}]) or []
        except Exception:
            hist = []
        for h in hist:
            sg = h.get("signature")
            if sg in (n_sig, ours):
                continue
            sl = h.get("slot")
            if sl is None:
                continue
            if n_slot <= sl <= (o_slot or sl):
                others.append({
                    "sig": sg,
                    "slot": sl,
                    "err": h.get("err"),
                })
    if why == "success":
        klass = "WIN"
    elif why in ("never_lands", "never_lands_swqos_fail"):
        klass = "OURS_NEVER_LANDS"
    elif why in ("stale_custom6", "TRIGGER_MISSING"):
        if any(x.get("err") is None for x in others):
            klass = "LOST_RACE"
        elif n_info.get("n_outcome") == "landed-success" and not others:
            klass = "CUSTOM6_NO_COMPETITOR"
        elif n_info.get("n_outcome") == "disappeared":
            klass = "TRIGGER_MISSING"
        else:
            klass = "CUSTOM6"
    else:
        klass = why.upper()
    tm = fire_out.get("timing") or {}
    out = {
        "class": klass,
        "N": {"sig": n_sig, "slot": n_slot, "outcome": n_info.get("n_outcome")},
        "OURS": {"sig": ours, "slot": o_slot, "err": o_row.get("err")},
        "competitor": others[:8],
        "ordering": {
            "N_slot": n_slot,
            "competitor_slots": [x.get("slot") for x in others[:8]],
            "OURS_slot": o_slot,
            "delta_ours_minus_N": (
                (o_slot - n_slot) if (o_slot and n_slot) else None
            ),
        },
        "timing": {
            "T_actionable": tm.get("T_actionable"),
            "T_framed": tm.get("T_framed"),
            "T_decision": tm.get("T_decision"),
            "T_signed": tm.get("T_signed"),
            "T_SWQOS_return": tm.get("T_SWQOS_return"),
        },
    }
    print(
        f"  CLASS {klass} N_slot={n_slot} OURS_slot={o_slot} "
        f"competitors={len(others)}",
        flush=True,
    )
    return out


def poll_sig(sig: str, seconds: int) -> dict:
    t0 = time.time()
    last = None
    while time.time() - t0 < seconds:
        st = d.rpc("getSignatureStatuses", [[sig], {"searchTransactionHistory": True}])
        last = (st.get("value") or [None])[0]
        if last and (last.get("err") is not None or last.get("confirmationStatus") in (
            "confirmed", "finalized",
        )):
            return last
        time.sleep(0.4)
    return last or {}


def fire(payer, rec: dict, send_lamports: int, est_gp: int) -> dict:
    t_decision = raw_ns()
    pool = str(rec.get("pool") or "")
    row = _plane.get(pool)
    if not row:
        raise e.AltPlaneMiss(f"no resident template {pool[:8]}")
    raw_dir = (rec.get("arb") or {}).get("direction")
    if raw_dir is None:
        raise RuntimeError("direction missing")
    direction = int(raw_dir)
    if direction not in (0, 1):
        raise RuntimeError(f"direction {direction}")
    tmpl = row["tmpl1"] if direction == 1 else row["tmpl0"]
    if not tmpl:
        raise e.AltPlaneMiss("dir template missing")
    bh = resident_bh()
    if len(bh) != 32:
        raise RuntimeError("no resident blockhash")
    buy = direction == 1
    tx = e.sign_v0(
        payer,
        e.patch(tmpl, direction, send_lamports, MIN_PROFIT, bh, CU_PRICE, buy=buy),
        buy=buy,
    )
    t_signed = raw_ns()
    sig = str(Signature.from_bytes(bytes(tx[1:65])))
    ARMED.unlink(missing_ok=True)
    rc, t_send0, t_send1 = racer_send(tx)
    t_swqos = raw_ns()
    print(f"RACER_SEND rc={rc} send_ns={t_send1 - t_send0} len={len(tx)}", flush=True)
    status = {}
    if rc == 0:
        status = poll_sig(sig, 75)
    fr = rec.get("frame") or {}
    tm = rec.get("timing") or {}
    return {
        "dlmm": row["dlmm"],
        "pump": row["pump"],
        "token": row.get("token"),
        "direction": direction,
        "send_lamports": send_lamports,
        "est_gp": est_gp,
        "min_profit": MIN_PROFIT,
        "cu_price": CU_PRICE,
        "tx_len": len(tx),
        "sig": sig,
        "alt": row.get("alt"),
        "path": "resident_template",
        "timing": {
            "clock": "CLOCK_MONOTONIC_RAW",
            "T_actionable": tm.get("actionable_ns"),
            "T_framed": fr.get("framed_ns"),
            "T_decision": t_decision,
            "T_signed": t_signed,
            "T_SWQOS_send": t_send0,
            "T_SWQOS_return": t_send1,
            "T_SWQOS_return_client": t_swqos,
            "frame_delay_ns": fr.get("delay_ns"),
            "decision_minus_framed_ns": (
                t_decision - int(fr.get("framed_ns") or t_decision)
            ),
            "sign_minus_decision_ns": t_signed - t_decision,
            "swqos_send_ns": t_send1 - t_send0,
        },
        "swqos_rc": rc,
        "status": status,
    }


def main() -> int:
    if not FUNDED:
        print(
            f"SEND=0  funded attempts stopped  hurdle={HURDLE} "
            f"(onchain={ONCHAIN_FEE}+swqos={SWQOS_UNIT}+safety={SAFETY})",
            flush=True,
        )
        return 0
    if not S007_READY.exists():
        print("STATE007 not READY — refuse to arm", flush=True)
        return 1
    live.load_dotenv()
    if os.environ.get("SWQOS_KEY") is None and os.environ.get("SWQOS_API_KEY") is None:
        sw = Path("/home/louis/.arb-swqos.env")
        if sw.exists():
            for line in sw.read_text(encoding="utf-8").splitlines():
                if line.startswith("export "):
                    line = line[7:]
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    os.environ.setdefault(k.strip(), v.strip().strip("\"'"))
    if os.environ.get("SWQOS_KEY") is None and os.environ.get("SWQOS_API_KEY") is None:
        print("missing SWQOS_KEY", flush=True)
        return 1
    if not SWQOS_SOCK.exists():
        print(f"missing racer socket {SWQOS_SOCK}", flush=True)
        return 1
    if not AUDIT.exists():
        print(f"missing {AUDIT}", flush=True)
        return 1
    if ARMED.exists():
        print("already armed — refuse", flush=True)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    payer = e.payer_kp()
    wallet = str(payer.pubkey())
    our_exec = e.program_v3()
    bal = d.rpc("getBalance", [wallet])
    lamports = bal["value"] if isinstance(bal, dict) else bal
    print(
        f"ONESHOT#6  RESIDENT  wallet={wallet} sol={lamports / 1e9:.6f} "
        f"cap={MAX_IN / 1e9:.2f} hurdle={HURDLE} exec={our_exec[:8]}",
        flush=True,
    )
    wsol_pk = e.ata(wallet, e.SOL, e.TOKENKEG)
    winfo = d.get_multiple([wsol_pk])[0]
    wsol = d.token_amount(winfo["data"]) if winfo else 0
    need_native = 30_000_000 if wsol >= MAX_IN else MAX_IN + 80_000_000
    if lamports < need_native:
        print(f"wallet too small native={lamports} wsol={wsol} need={need_native}", flush=True)
        return 1

    e.ensure_ata(payer, wallet, e.SOL, e.TOKENKEG)
    e.wrap_wsol(payer, e.ata(wallet, e.SOL, e.TOKENKEG), MAX_IN)

    global _plane
    _plane = load_race_plane()
    if not _plane:
        print("no RACE_READY templates in plane — control plane must publish tmpl0/tmpl1", flush=True)
        return 1
    threading.Thread(target=hash_loop, name="bh", daemon=True).start()
    for _ in range(40):
        if len(resident_bh()) == 32:
            break
        time.sleep(0.1)
    if len(resident_bh()) != 32:
        print("resident blockhash not ready", flush=True)
        return 1
    print(f"  plane_routes={len(_plane)//2} resident_bh=1 racer={SWQOS_SOCK}", flush=True)

    ARMED.write_text("1\n", encoding="utf-8")
    if DISARMED.exists():
        DISARMED.unlink()
    print(f"ARMED  follow {AUDIT} from EOF", flush=True)

    t0 = time.time()
    last_hb = t0
    seen = 0
    try:
        for line in follow_new(AUDIT):
            if time.time() - t0 > WAIT_S:
                disarm("timeout", {"seen": seen})
                return 0
            if time.time() - last_hb >= HEARTBEAT_S:
                print(
                    f"  wait {int(time.time() - t0)}s seen={seen} still armed",
                    flush=True,
                )
                last_hb = time.time()
            if line is None:
                continue
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") != "opp_synced":
                continue
            if not framed_ready(rec):
                print(
                    f"  skip not CORE010_FRAMED class={(rec.get('frame') or {}).get('class')} "
                    f"race_ready={rec.get('race_ready')}",
                    flush=True,
                )
                continue
            seen += 1
            arb = rec.get("arb") or {}
            gate = size_gate(rec)
            send_journal(rec, gate)
            print(
                f"  opp pool={(rec.get('pool') or '')[:8]} dir={arb.get('direction')} "
                f"ain={arb.get('amount_in')} gp={arb.get('gross')} gate={gate}",
                flush=True,
            )
            if gate is None:
                continue
            send, est = gate
            try:
                out = fire(payer, rec, send, est)
            except e.AltPlaneMiss as ex:
                print(f"  skip ALT_PLANE {ex}", flush=True)
                continue
            except Exception as ex:
                disarm("setup_or_send_fail", {"error": str(ex)[:300], "rec": rec})
                return 1
            err = (out.get("status") or {}).get("err")
            conf = (out.get("status") or {}).get("confirmationStatus")
            ie = err.get("InstructionError") if isinstance(err, dict) else None
            if out.get("swqos_rc") != 0:
                why = "never_lands_swqos_fail"
            elif not out.get("status"):
                why = "never_lands"
            elif isinstance(ie, list) and len(ie) >= 2 and ie[1] == {"Custom": 6}:
                why = "stale_custom6"
            elif err:
                why = "program_error"
            elif conf in ("confirmed", "finalized"):
                why = "success"
            else:
                why = "never_lands"
            n_hex = rec.get("sig_hex") or ""
            n_info = {"n_sig": None, "n_status": None, "n_outcome": None}
            if n_hex:
                n_sig = str(Signature.from_bytes(bytes.fromhex(n_hex)))
                n_st = d.rpc(
                    "getSignatureStatuses",
                    [[n_sig], {"searchTransactionHistory": True}],
                )
                n_ent = ((n_st or {}).get("value") or [None])[0]
                n_info = {
                    "n_sig": n_sig,
                    "n_status": n_ent,
                    "n_outcome": (
                        "disappeared" if not n_ent
                        else ("landed-failed" if n_ent.get("err") else "landed-success")
                    ),
                }
                if why == "stale_custom6" and n_info["n_outcome"] == "disappeared":
                    why = "TRIGGER_MISSING"
                    n_info["cooldown"] = note_trigger_missing(str(rec.get("pool") or ""))
            race = classify_race(rec, out, why, n_info)
            disarm(why, {"opp": rec, "fire": out, "trigger": n_info, "race": race})
            return 0
    except KeyboardInterrupt:
        disarm("interrupted", {"seen": seen})
        return 1
    disarm("timeout", {"seen": seen})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
