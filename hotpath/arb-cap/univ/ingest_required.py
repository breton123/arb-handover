#!/usr/bin/env python3
"""Build the next liveuniv generation from the full real pool universe.

Sources (union, no popularity prune):
  required_pools.json   528 proven market-hour DLMM/Pump pools
  current liveuniv.bin  already-bootstrapped records (kept as raw blobs)
  discover.jsonl        OF/FRAMED pools the searcher journaled as unknown

RPC/Shyft bootstrap is off-path. The C compiler decides whether a pool
contributes a valid route. We do not drop a pool because it was unpopular.
"""
from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "state005"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
from rebuild_univ import fetch_priced  # noqa: E402

REQUIRED = Path(__file__).resolve().parents[1] / "trigger012" / "required_pools.json"
UNIV_BIN = Path("/home/louis/captures/paper_orbit/liveuniv.bin")
UNIV_JS = Path("/home/louis/captures/paper_orbit/liveuniv.json")
DISCOVER = Path("/home/louis/captures/paper_orbit/discover.jsonl")
OUT_DIR = Path("/home/louis/captures/paper_orbit")
HARD_MAX = 65535


def b58(hex32: str) -> str:
    return live.b58e(bytes.fromhex(hex32))


def read_existing(path: Path) -> tuple[list[dict], int]:
    if not path.exists():
        return [], 0
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
        off += 161
        if proto == live.PROTO_DLMM:
            nbin = struct.unpack_from("<H", b, off + 71)[0]
            off += 73 + nbin * 20
        elif proto == live.PROTO_PUMP:
            off += 50
        else:
            raise SystemExit(f"unsupported proto {proto}")
        recs.append({"kind": "dlmm" if proto == live.PROTO_DLMM else "pump",
                     "pk": pk, "raw": b[start:off], "slot": slot})
    return recs, slot


def load_required(path: Path) -> list[dict]:
    if not path.exists():
        return []
    obj = json.loads(path.read_text(encoding="utf-8"))
    return list(obj.get("pools") or [])


def load_discover(path: Path) -> list[dict]:
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        hx = r.get("pool_hex") or ""
        if len(hx) != 64:
            continue
        proto = int(r.get("proto") or 0)
        kind = "dlmm" if proto == live.PROTO_DLMM else "pump" if proto == live.PROTO_PUMP else None
        if kind is None:
            continue
        rows.append({"pool": b58(hx), "proto": kind})
    return rows


def write_univ(items: list, out: Path, slot: int) -> None:
    if len(items) > HARD_MAX:
        raise SystemExit(f"n={len(items)} exceeds uint16 live.bin hard max {HARD_MAX}")
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


def main() -> int:
    live.load_dotenv()
    req_path = Path(sys.argv[1]) if len(sys.argv) > 1 else REQUIRED
    out_bin = Path(sys.argv[2]) if len(sys.argv) > 2 else UNIV_BIN
    existing, old_slot = read_existing(out_bin if out_bin.exists() else UNIV_BIN)
    have = {e["pk"] for e in existing}
    items = list(existing)
    want: list[tuple[str, str]] = []
    for p in load_required(req_path):
        pk = p.get("pool") or p.get("pubkey")
        kind = (p.get("proto") or p.get("kind") or "").lower()
        if pk and kind in ("dlmm", "pump") and pk not in have:
            want.append((kind, pk))
    for p in load_discover(DISCOVER):
        pk, kind = p["pool"], p["proto"]
        if pk not in have:
            want.append((kind, pk))
    # de-dup preserving order
    seen = set(have)
    ordered = []
    for kind, pk in want:
        if pk in seen:
            continue
        seen.add(pk)
        ordered.append((kind, pk))
    print(
        f"INGEST  have={len(items)} required_new={sum(1 for k,_ in ordered)} "
        f"hard_max={HARD_MAX} no popularity prune",
        flush=True,
    )
    gcfg_pk = d._pk(d.find_pda([b"global_config"], live.b58d(live.PUMP)))
    gacc = d.get_multiple([gcfg_pk])[0]
    fees = live.parse_global(gacc["data"]) if gacc else (20, 5, 0, 0)
    added = 0
    failed = 0
    for i, (kind, pk) in enumerate(ordered):
        if len(items) >= HARD_MAX:
            print(f"INGEST  hard-max {HARD_MAX} hit; remaining deferred", flush=True)
            break
        time.sleep(0.12)
        try:
            if kind == "dlmm":
                row = fetch_priced(pk)
            else:
                row = live.fetch_pump(pk, fees)
        except Exception as e:
            print(f"  fail {kind} {pk[:8]} {type(e).__name__}", flush=True)
            failed += 1
            continue
        if row is None:
            print(f"  skip {kind} {pk[:8]} no snap", flush=True)
            failed += 1
            continue
        items.append({"kind": kind, "row": row, "pk": pk})
        added += 1
        if added % 25 == 0 or i + 1 == len(ordered):
            print(f"  fetched {added}/{len(ordered)} n={len(items)}", flush=True)
    slot = max(
        [old_slot]
        + [int(it.get("slot") or 0) for it in items]
        + [int((it.get("row") or {}).get("slot") or 0) for it in items],
    )
    write_univ(items, out_bin, slot)
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import rewrite_univ_json  # noqa: E402
    bin_pools, _ = rewrite_univ_json.pools_from_bin(out_bin)
    js = {
        "path": str(out_bin),
        "n": len(items),
        "n_dlmm": sum(1 for it in items if it.get("kind") == "dlmm"),
        "n_pump": sum(1 for it in items if it.get("kind") == "pump"),
        "added": added,
        "failed": failed,
        "slot": slot,
        "hard_max": HARD_MAX,
        "popularity_prune": False,
        "pools": bin_pools,
    }
    UNIV_JS.parent.mkdir(parents=True, exist_ok=True)
    UNIV_JS.write_text(json.dumps(js, indent=2) + "\n", encoding="utf-8")
    print(
        f"INGEST  wrote {out_bin} n={js['n']} dlmm={js['n_dlmm']} pump={js['n_pump']} "
        f"added={added} failed={failed}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
