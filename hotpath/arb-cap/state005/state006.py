#!/usr/bin/env python3
"""STATE-006 — off-hot-path land check + authoritative snap. No send.

Writes recon.bin for paper_orbit: REJECT / LAND / AUTH.
Does not call hot_commit. Does not touch feed_live.
"""
from __future__ import annotations

import io
import json
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "paper004"))
import live001 as live  # noqa: E402
import paper004 as p4  # noqa: E402
import record_dlmm as d  # noqa: E402
from rebuild_univ import fetch_priced  # noqa: E402

PEND = Path("/home/louis/captures/paper_orbit/pending.jsonl")
RECON = Path("/home/louis/captures/paper_orbit/recon.bin")
UNIV = Path("/home/louis/captures/paper_orbit/liveuniv.json")
LOG = Path("/home/louis/captures/paper_orbit/state006.log")

RECON_MAGIC = 0x36305453
RECON_REJECT = 1
RECON_LAND = 2
RECON_AUTH = 3
TIMEOUT_S = 15.0
REFRESH_S = 2.0
FRAMED_RANK = Path("/home/louis/captures/paper_orbit/FRAMED_POOLS.json")
REFRESH_EXTRA = 24
STATE007_READY = Path("/home/louis/captures/state007/READY")


def load_univ() -> dict:
    return json.loads(UNIV.read_text(encoding="utf-8"))


def pool_of(univ: dict, idx: int) -> dict | None:
    for p in univ.get("pools") or []:
        if int(p.get("idx") or -1) == idx:
            return p
    return None


def write_hdr(f) -> None:
    f.write(struct.pack("<IHH", RECON_MAGIC, 1, 0))
    f.flush()
    os.fsync(f.fileno())


def write_rec(f, kind: int, proto: int, pool_idx: int, sig: bytes, slot: int, body: bytes = b"") -> None:
    fixed = struct.pack("<BBHI", kind, proto, 0, pool_idx) + sig + struct.pack("<Q", slot)
    if len(fixed) != 80:
        raise RuntimeError("recon fixed")
    f.write(struct.pack("<I", 80 + len(body)))
    f.write(fixed)
    f.write(body)
    f.flush()
    os.fsync(f.fileno())


def dlmm_body(row: dict) -> bytes:
    buf = io.BytesIO()
    live.write_dlmm(buf, row["snap"])
    return buf.getvalue()


def pump_body(row: dict) -> bytes:
    buf = io.BytesIO()
    live.write_pump(buf, row)
    return buf.getvalue()


def _plane_keys() -> set[str]:
    p = Path("/home/louis/arb-cap/oneshot/ALT_PLANE.json")
    if not p.exists():
        return set()
    try:
        rows = json.loads(p.read_text(encoding="utf-8")).get("routes") or []
    except Exception:
        return set()
    out = set()
    for r in rows:
        if r.get("RACE_READY"):
            if r.get("dlmm"):
                out.add(r["dlmm"])
            if r.get("pump"):
                out.add(r["pump"])
    return out


def _framed_freq() -> dict[str, int]:
    if not FRAMED_RANK.exists():
        return {}
    try:
        rows = json.loads(FRAMED_RANK.read_text(encoding="utf-8")).get("rows") or []
    except Exception:
        return {}
    return {str(r.get("pool") or ""): int(r.get("n") or 0) for r in rows if r.get("pool")}


def _hot_pubkeys() -> set[str]:
    """Every liveuniv DLMM/Pump. 256-cap set is one/two getMultiple chunks.

    #6 plane stays included. Searchable 3-hop DLMMs were aging out at 35–42
    slots because they were not on the old plane+24 list.
    """
    out = _plane_keys()
    try:
        univ = load_univ()
    except Exception:
        univ = {}
    for p in univ.get("pools") or []:
        kind = str(p.get("proto") or p.get("kind") or "")
        pk = p.get("pubkey")
        if pk and kind in ("dlmm", "pump"):
            out.add(pk)
    return out


def refresh_route0(rf, univ: dict, fees) -> int:
    """One/two getMultipleAccounts for the hot set. Never called from the race."""
    hot = _hot_pubkeys()
    rows = []
    for p in univ.get("pools") or []:
        pk = p.get("pubkey")
        kind = str(p.get("proto") or p.get("kind") or "")
        idx = int(p.get("idx") if p.get("idx") is not None else -1)
        if not pk or idx < 0 or kind not in ("dlmm", "pump"):
            continue
        if hot and pk not in hot:
            continue
        rows.append((idx, kind, pk))
    if not rows:
        return 0
    keys1 = [pk for _, _, pk in rows]
    try:
        accs1 = d.get_multiple(keys1, retries=2)
    except Exception as ex:
        print(f"  REFRESH_FAIL pools {type(ex).__name__}", flush=True)
        return 0
    by = {pk: accs1[i] if i < len(accs1) else None for i, pk in enumerate(keys1)}
    extra: list[str] = []
    work = []
    for idx, kind, pk in rows:
        a = by.get(pk)
        if not a:
            continue
        try:
            if kind == "dlmm":
                lb = d.parse_lbpair(a["data"])
                bins = [d.bin_array_pda(pk, i) for i in d.array_indexes(lb["active_id"])]
                extra.extend(bins)
                work.append((idx, kind, pk, a, lb, bins))
            else:
                p = live.parse_pump_pool(a["data"])
                if not p:
                    continue
                vb = live.b58e(p["vault_base"])
                vq = live.b58e(p["vault_quote"])
                extra.extend([vb, vq])
                work.append((idx, kind, pk, a, p, [vb, vq]))
        except Exception as ex:
            print(f"  REFRESH_FAIL {pk[:8]} {type(ex).__name__}", flush=True)
    extra = list(dict.fromkeys(extra))
    by2: dict[str, dict | None] = {}
    if extra:
        try:
            accs2 = d.get_multiple(extra, retries=2)
            by2 = {k: accs2[i] if i < len(accs2) else None for i, k in enumerate(extra)}
        except Exception as ex:
            print(f"  REFRESH_FAIL extras {type(ex).__name__}", flush=True)
            return 0
    n = 0
    lp, proto_fee, creator, disabled = fees
    for idx, kind, pk, a, meta, kids in work:
        try:
            if kind == "dlmm":
                bins = []
                for k in kids:
                    acc = by2.get(k)
                    if acc and acc.get("data"):
                        bins.extend(d.parse_bin_array(acc["data"]))
                active = meta["active_id"]
                use = [b for b in bins if abs(b["id"] - active) <= live.K]
                snap = {
                    "lb": meta,
                    "bins": use,
                    "reserve_x": sum(b["x"] for b in use),
                    "reserve_y": sum(b["y"] for b in use),
                }
                write_rec(
                    rf, RECON_AUTH, live.PROTO_DLMM, idx, bytes(64),
                    int(a.get("slot") or 0), dlmm_body({"snap": snap}),
                )
                n += 1
            else:
                vb, vq = kids
                av = by2.get(vb)
                aq = by2.get(vq)
                rb = d.token_amount(av["data"]) if av and av.get("data") else 0
                rq = d.token_amount(aq["data"]) if aq and aq.get("data") else 0
                if rb == 0 or rq == 0:
                    continue
                row = {
                    "reserve_base": rb,
                    "reserve_quote": rq,
                    "virtual_quote": 0,
                    "lp_fee_bps": lp,
                    "protocol_fee_bps": proto_fee,
                    "creator_fee_bps": creator,
                    "disabled": disabled,
                    "status": 1 if meta.get("mayhem") else 0,
                }
                write_rec(
                    rf, RECON_AUTH, live.PROTO_PUMP, idx, bytes(64),
                    int(a.get("slot") or 0), pump_body(row),
                )
                n += 1
        except Exception as ex:
            print(f"  REFRESH_FAIL {pk[:8]} {type(ex).__name__}", flush=True)
    return n


def statuses(sigs: list[str]) -> list[dict | None]:
    """One getSignatureStatuses per 256 sigs. Never per-sig getTransaction."""
    out: list[dict | None] = []
    for i in range(0, len(sigs), 256):
        chunk = sigs[i : i + 256]
        res = d.rpc(
            "getSignatureStatuses",
            [chunk, {"searchTransactionHistory": True}],
            retries=2,
            backoff=2.0,
        )
        vals = (res or {}).get("value") or []
        out.extend(vals)
        if len(vals) < len(chunk):
            out.extend([None] * (len(chunk) - len(vals)))
    return out


def status_kind(v: dict | None) -> tuple[str, int]:
    if not v:
        return "gone", 0
    slot = int(v.get("slot") or 0)
    if v.get("err"):
        return "fail", slot
    return "ok", slot


def main() -> int:
    live.load_dotenv()
    RECON.parent.mkdir(parents=True, exist_ok=True)
    if not RECON.exists() or RECON.stat().st_size == 0:
        with RECON.open("wb") as f:
            write_hdr(f)
    pend_off = 0
    if os.environ.get("STATE006_FROM_START") != "1" and PEND.exists():
        pend_off = PEND.stat().st_size
    seen: dict[str, dict] = {}
    print("STATE-006  follow pending  recon AUTH after LAND  hot refresh  NO SEND", flush=True)
    rf = RECON.open("ab")
    gcfg_pk = d._pk(d.find_pda([b"global_config"], live.b58d(live.PUMP)))
    gacc = d.get_multiple([gcfg_pk])[0]
    fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)
    last_refresh = 0.0
    while True:
        univ = load_univ()
        if PEND.exists() and PEND.stat().st_size < pend_off:
            pend_off = 0
            seen.clear()
            print("STATE-006  pending truncated — rewind", flush=True)
        if PEND.exists():
            raw = PEND.read_bytes()
            if len(raw) > pend_off:
                chunk = raw[pend_off:]
                pend_off = len(raw)
                for line in chunk.splitlines():
                    if not line.strip():
                        continue
                    try:
                        row = json.loads(line)
                    except Exception:
                        continue
                    hx = row.get("sig_hex") or ""
                    if not hx or hx in seen:
                        continue
                    try:
                        sigb = bytes.fromhex(hx)
                        sig = p4.b58encode(sigb)
                    except Exception:
                        continue
                    seen[hx] = {
                        "sig": sig,
                        "sigb": sigb,
                        "pool_idx": int(row.get("pool_idx") or 0),
                        "proto": int(row.get("proto") or 0),
                        "t0": time.time(),
                        "done": False,
                    }
        now = time.time()
        undone = [st for st in seen.values() if not st["done"]]
        batch: list[dict | None] = []
        if undone:
            try:
                batch = statuses([st["sig"] for st in undone])
            except Exception as ex:
                print(f"  STATUS_FAIL {type(ex).__name__}", flush=True)
                batch = [None] * len(undone)
        for st, v in zip(undone, batch):
            kind, slot = status_kind(v)
            if kind == "gone" and now - st["t0"] < TIMEOUT_S:
                continue
            pidx = st["pool_idx"]
            proto = st["proto"]
            if kind != "ok":
                write_rec(rf, RECON_REJECT, proto, pidx, st["sigb"], slot)
                st["done"] = True
                print(f"  REJECT {st['sig'][:8]} {kind} pool={pidx}", flush=True)
                continue
            write_rec(rf, RECON_LAND, proto, pidx, st["sigb"], slot)
            print(f"  LAND   {st['sig'][:8]} slot={slot} pool={pidx}", flush=True)
            meta = pool_of(univ, pidx)
            pk = (meta or {}).get("pubkey")
            body = b""
            try:
                if proto == live.PROTO_DLMM and pk:
                    row = fetch_priced(pk)
                    if row:
                        body = dlmm_body(row)
                        slot = int(row.get("slot") or slot)
                elif proto == live.PROTO_PUMP and pk:
                    row = live.fetch_pump(pk, fees)
                    if row:
                        body = pump_body(row)
                        slot = int(row.get("slot") or slot)
            except Exception as e:
                print(f"  SNAPFAIL {st['sig'][:8]} {type(e).__name__}", flush=True)
            if body:
                write_rec(rf, RECON_AUTH, proto, pidx, st["sigb"], slot, body)
                print(f"  AUTH   {st['sig'][:8]} bytes={len(body)}", flush=True)
            st["done"] = True
        if now - last_refresh >= REFRESH_S:
            if STATE007_READY.exists():
                print("  REFRESH skip STATE-007 owns AUTH", flush=True)
            else:
                n = refresh_route0(rf, univ, fees)
                print(f"  REFRESH n={n}", flush=True)
            last_refresh = now
        time.sleep(0.25)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
