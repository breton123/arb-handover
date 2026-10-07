#!/usr/bin/env python3
"""Ghost-N fingerprint for oneshot #1 / #4. Read-only. No send. No .cap write."""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, "/home/louis/arb-cap")
sys.path.insert(0, "/home/louis/arb-cap/paper004")
import live001 as live
import paper004 as p4

OUT = Path("/home/louis/arb-cap/oneshot/GHOST_N.json")
PEND = Path("/home/louis/captures/paper_orbit/pending.jsonl")
AUDIT = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
S006 = Path("/home/louis/captures/paper_orbit/state006.log")
RESULT = Path("/home/louis/arb-cap/oneshot/RESULT.json")
STALE = Path("/home/louis/arb-cap/oneshot/STALE.json")
CAP_DIR = Path("/home/louis/captures/orbitflare")
FEEDCAP_REC_HDR = 24
SHRED_OFF_VARIANT = 64
SHRED_OFF_SLOT = 65
SHRED_OFF_INDEX = 73
SHRED_OFF_VERSION = 77
SHRED_OFF_FEC = 79

CASES = [
    {
        "id": 1,
        "n": "3WzxXps9gBMijAxsvUcaSwJYwBq385s2wwLwCU23JSSLXvrsoTuJfHUDKAQfmCWYXG89nmjmYxAETyLxw3E8UtmM",
        "ours": "3q6BM1m6EJkLVvXBbF8haSMnk6msVCY7U1awYoX96ufodD8byML3Nhct2k267cqsrxWafwTeFG2k1HWENbvDg2kr",
        "slot_hint": 450389301,
        "pool": "Ftjga524YrS7RPzGCcezMuQnFDYa8PcvWDY5mFmZk5dy",
    },
    {
        "id": 4,
        "n": None,
        "ours": "yn7yJjDa1SLChivbUyDY7J7Y3vZxkYs7ZGDb1wvUjXngAD53Cv5WqSGqKSN7KC1STNZ5mgKbKq7Xpf9NRwdyzfk",
        "slot_hint": 450394596,
        "pool": "9PYge8aB",
    },
]


def b58_to_raw(sig: str) -> bytes:
    return p4.b58decode(sig)


def hex_of(sig: str) -> str:
    return b58_to_raw(sig).hex()


def grep_jsonl(path: Path, needle: str, limit: int = 8) -> list[dict]:
    hits = []
    if not path.exists() or not needle:
        return hits
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if needle not in line:
                continue
            line = line.strip()
            if not line:
                continue
            try:
                hits.append(json.loads(line))
            except json.JSONDecodeError:
                hits.append({"raw": line[:400]})
            if len(hits) >= limit:
                break
    return hits


def grep_text(path: Path, needle: str, limit: int = 12) -> list[str]:
    hits = []
    if not path.exists() or not needle:
        return hits
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if needle in line:
                hits.append(line.strip()[:400])
                if len(hits) >= limit:
                    break
    return hits


def parse_shred(data: bytes) -> dict | None:
    if len(data) < 83:
        return None
    typ = data[SHRED_OFF_VARIANT]
    slot = struct.unpack_from("<Q", data, SHRED_OFF_SLOT)[0]
    index = struct.unpack_from("<I", data, SHRED_OFF_INDEX)[0]
    fec = struct.unpack_from("<I", data, SHRED_OFF_FEC)[0]
    ver = struct.unpack_from("<H", data, SHRED_OFF_VERSION)[0]
    kind = "data" if (typ & 0xF0) in (0xA0, 0x80, 0x90, 0xB0) else (
        "code" if (typ & 0xF0) in (0x50, 0x40, 0x60, 0x70) else "other"
    )
    return {"slot": slot, "index": index, "fec": fec, "type": typ, "kind": kind, "ver": ver}


def scan_caps(sig_raw: bytes, max_hits: int = 6) -> list[dict]:
    hits = []
    if not CAP_DIR.exists() or len(sig_raw) != 64:
        return hits
    caps = sorted(CAP_DIR.glob("orbitflare-*.cap"), key=lambda p: p.stat().st_mtime)[-4:]
    for cap in caps:
        try:
            f = cap.open("rb")
        except OSError:
            continue
        with f:
            magic = f.read(8)
            if magic != b"FEEDCAP1":
                continue
            f.seek(40)
            while True:
                rec_off = f.tell()
                rec = f.read(FEEDCAP_REC_HDR)
                if len(rec) < FEEDCAP_REC_HDR:
                    break
                rx_ns, _rx_tsc, plen, seq = struct.unpack("<QQII", rec)
                if plen < 83 or plen > 1228:
                    f.seek(rec_off + 1)
                    continue
                pkt = f.read(plen)
                if len(pkt) < plen:
                    break
                pos = pkt.find(sig_raw)
                if pos < 0:
                    continue
                shred = parse_shred(pkt)
                hits.append({
                    "cap": cap.name,
                    "cap_off": rec_off,
                    "payload_off": pos,
                    "rx_ns": rx_ns,
                    "seq": seq,
                    "pkt_len": plen,
                    "shred": shred,
                })
                if len(hits) >= max_hits:
                    return hits
    return hits


def fetch_block(slot: int):
    try:
        return p4.rpc(
            "getBlock",
            [slot, {
                "encoding": "json",
                "transactionDetails": "signatures",
                "rewards": False,
                "maxSupportedTransactionVersion": 1,
            }],
            pause=0.3,
        )
    except Exception as ex:
        return {"_err": str(ex)[:200]}


def in_block(blk, sig: str) -> dict:
    if not isinstance(blk, dict) or blk.get("_err"):
        return {"present": False, "error": (blk or {}).get("_err")}
    sigs = blk.get("signatures") or []
    if sig in sigs:
        return {"present": True, "tx_index": sigs.index(sig), "n_tx": len(sigs)}
    return {"present": False, "n_tx": len(sigs)}


def rpc_n(sig: str) -> dict:
    st = p4.rpc("getSignatureStatuses", [[sig], {"searchTransactionHistory": True}], pause=0.2)
    ent = ((st or {}).get("value") or [None])[0]
    tx = None
    try:
        tx = p4.fetch_tx(sig)
    except Exception:
        tx = None
    out = {
        "status": ent,
        "tx_present": bool(tx),
        "slot": (tx or {}).get("slot") if tx else None,
        "err": ((tx or {}).get("meta") or {}).get("err") if tx else (ent or {}).get("err") if ent else None,
        "fee": ((tx or {}).get("meta") or {}).get("fee") if tx else None,
        "cu": ((tx or {}).get("meta") or {}).get("computeUnitsConsumed") if tx else None,
    }
    if not ent and not tx:
        out["rpc"] = "absent"
    elif ent and ent.get("err"):
        out["rpc"] = "landed-failed"
    elif tx and ((tx.get("meta") or {}).get("err")):
        out["rpc"] = "landed-failed"
    else:
        out["rpc"] = "landed-success"
    return out


def hydrate_case4(case: dict) -> dict:
    if not RESULT.exists():
        return case
    res = json.loads(RESULT.read_text(encoding="utf-8"))
    opp = res.get("opp") or {}
    fire = res.get("fire") or {}
    hx = opp.get("sig_hex") or ""
    if hx:
        try:
            case["n"] = p4.b58encode(bytes.fromhex(hx))
        except Exception:
            pass
    if fire.get("sig"):
        case["ours"] = fire["sig"]
    if fire.get("dlmm"):
        case["pool"] = fire.get("dlmm")
    case["opp_n"] = opp.get("n")
    case["arb"] = opp.get("arb")
    case["send_quote"] = opp.get("send_quote")
    case["shred"] = opp.get("shred")
    case["n_ix"] = opp.get("n_ix")
    return case


def main() -> int:
    live.load_dotenv()
    cases = [dict(c) for c in CASES]
    cases[1] = hydrate_case4(cases[1])
    report = {"cost": json.loads(Path("/home/louis/arb-cap/oneshot/COST.json").read_text())
              if Path("/home/louis/arb-cap/oneshot/COST.json").exists() else {},
              "cases": []}
    for case in cases:
        n = case.get("n")
        ours = case.get("ours")
        row = {"id": case["id"], "n": n, "ours": ours, "pool": case.get("pool")}
        if not n:
            row["error"] = "N sig unknown"
            report["cases"].append(row)
            continue
        hx = hex_of(n)
        row["n_hex8"] = hx[:16]
        row["rpc"] = rpc_n(n)
        ours_tx = None
        try:
            ours_tx = p4.fetch_tx(ours) if ours else None
        except Exception:
            ours_tx = None
        our_slot = (ours_tx or {}).get("slot") or case.get("slot_hint")
        row["ours_slot"] = our_slot
        row["ours_err"] = ((ours_tx or {}).get("meta") or {}).get("err") if ours_tx else None
        row["ours_fee"] = ((ours_tx or {}).get("meta") or {}).get("fee") if ours_tx else None
        row["ours_cu"] = ((ours_tx or {}).get("meta") or {}).get("computeUnitsConsumed") if ours_tx else None
        row["block"] = {}
        if our_slot:
            blk = fetch_block(int(our_slot))
            row["block"][str(our_slot)] = in_block(blk, n)
            for dlt in (-1, 1):
                b2 = fetch_block(int(our_slot) + dlt)
                row["block"][str(int(our_slot) + dlt)] = in_block(b2, n)
        row["pending"] = grep_jsonl(PEND, hx)
        row["opp_synced"] = grep_jsonl(AUDIT, hx)
        row["state006"] = grep_text(S006, n[:12]) + grep_text(S006, hx[:16])
        if case.get("opp_n"):
            row["n_fields"] = case["opp_n"]
        elif row["opp_synced"]:
            row["n_fields"] = (row["opp_synced"][0] or {}).get("n")
        row["n_ix"] = case.get("n_ix") or ((row["opp_synced"][0] or {}).get("n_ix") if row["opp_synced"] else None)
        row["audit_shred"] = case.get("shred") or ((row["opp_synced"][0] or {}).get("shred") if row["opp_synced"] else None)
        row["cap_hits"] = scan_caps(b58_to_raw(n))
        row["fingerprint"] = {
            "rpc_absent": row["rpc"].get("rpc") == "absent",
            "not_in_our_slot_block": not (row["block"].get(str(our_slot)) or {}).get("present"),
            "seen_in_pending": bool(row["pending"]),
            "state006_reject_gone": any("REJECT" in x and "gone" in x for x in row["state006"]),
            "seen_in_cap": bool(row["cap_hits"]),
            "shred_kind": (row["cap_hits"][0].get("shred") or {}).get("kind") if row["cap_hits"] else None,
            "shred_index": (row["cap_hits"][0].get("shred") or {}).get("index") if row["cap_hits"] else None,
        }
        print(json.dumps({"id": row["id"], "rpc": row["rpc"]["rpc"], "fp": row["fingerprint"]}, indent=2), flush=True)
        report["cases"].append(row)
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("WROTE", OUT, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
