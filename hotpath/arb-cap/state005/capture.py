#!/usr/bin/env python3
"""STATE-005 — capture real DLMM (S_before, N, S_after) from OrbitFlare N.

On shred-seen N, RPC-fetch pricing state immediately (bin-sum, not vault).
When N lands, fetch after + Swap2Evt. If before.active == event.start_bin_id,
write a triple and run frozen dlmm_apply_swap. No send.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "state004"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402
import decomp as ev  # noqa: E402

K = 64
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58e(raw: bytes) -> str:
    n = int.from_bytes(raw, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58[r] + out
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return "1" * pad + (out or "1")


def write_state(f, snap: dict) -> None:
    lb = snap["lb"]
    bins = snap["bins"]
    f.write(
        f'{lb["active_id"]} {lb["bin_step"]} {lb["status"]} '
        f'{lb["base_factor"]} {lb["filter_period"]} {lb["decay_period"]} '
        f'{lb["reduction_factor"]} {lb["variable_fee_control"]} '
        f'{lb["max_volatility_accumulator"]} {lb["protocol_share"]} '
        f'{lb["base_fee_power_factor"]} {lb["collect_fee_mode"]} '
        f'{lb["vol_acc"]} {lb["vol_ref"]} {lb["idx_ref"]} {lb["last_upd"]} '
        f'{snap["reserve_x"]} {snap["reserve_y"]} {len(bins)}\n'
    )
    for b in bins:
        f.write(f'{b["id"]} {b["x"]} {b["y"]}\n')


def pack_tri(path: Path, n: dict, before: dict, after: dict, event: dict) -> None:
    now_ts = int(n.get("block_time") or int(time.time()))
    with path.open("w", encoding="utf-8") as f:
        f.write("ST5\n")
        f.write(
            f'N {int(n["n_dir"])} {int(n["n_ain"])} {int(n.get("min_out") or 0)} {now_ts}\n'
        )
        f.write("BEFORE\n")
        write_state(f, before)
        f.write("AFTER\n")
        write_state(f, after)
        f.write(
            f'EVENT {int(event["amount_out"])} {int(event["fee"])} '
            f'{int(event["protocol_fee"])} {int(event["start_bin_id"])} '
            f'{int(event["end_bin_id"])}\n'
        )


def fetch_tx(sig: str):
    return ev.rpc(
        "getTransaction",
        [sig, {"encoding": "jsonParsed", "maxSupportedTransactionVersion": 1}],
    )


def is_6001(tx: dict) -> bool:
    err = (tx.get("meta") or {}).get("err")
    return err is not None and "6001" in json.dumps(err)


def snap_json(snap: dict) -> dict:
    lb = snap["lb"]
    return {
        "slot": snap["slot"],
        "active_id": lb["active_id"],
        "vol_acc": lb["vol_acc"],
        "vol_ref": lb["vol_ref"],
        "idx_ref": lb["idx_ref"],
        "last_upd": lb["last_upd"],
        "bin_step": lb["bin_step"],
        "reserve_x": snap["reserve_x"],
        "reserve_y": snap["reserve_y"],
        "vault_x": snap.get("vault_x"),
        "vault_y": snap.get("vault_y"),
        "nbin": len(snap["bins"]),
        "bins": snap["bins"],
        "parameters": {
            "base_factor": lb["base_factor"],
            "filter_period": lb["filter_period"],
            "decay_period": lb["decay_period"],
            "reduction_factor": lb["reduction_factor"],
            "variable_fee_control": lb["variable_fee_control"],
            "max_volatility_accumulator": lb["max_volatility_accumulator"],
            "protocol_share": lb["protocol_share"],
            "base_fee_power_factor": lb["base_fee_power_factor"],
            "collect_fee_mode": lb["collect_fee_mode"],
        },
    }


def walk_len(event: dict | None) -> int | None:
    if not event:
        return None
    try:
        return abs(int(event["end_bin_id"]) - int(event["start_bin_id"]))
    except Exception:
        return None


def after_active(after: dict) -> int | None:
    if not after:
        return None
    if "active_id" in after:
        return int(after["active_id"])
    lb = after.get("lb") or {}
    if "active_id" in lb:
        return int(lb["active_id"])
    return None


def _line_u64(out: str, key: str) -> int | None:
    for ln in out.splitlines():
        if key in ln:
            for tok in ln.replace("=", " ").split():
                if tok.isdigit():
                    return int(tok)
    return None


def classify_compare(cmp_out: str, cmp_rc: int | None, after: dict, event: dict) -> str:
    end = int(event["end_bin_id"])
    aa = after_active(after)
    if aa is not None and aa != end:
        return "stale_after"
    out = cmp_out or ""
    if cmp_rc == 2 or "APPLY_FAIL" in out:
        return "apply_fail"
    if cmp_rc == 0:
        return "clean"
    ev_out, ev_fee = int(event["amount_out"]), int(event["fee"])
    pred_out = _line_u64(out, "pred   out=")
    pred_fee = None
    for ln in out.splitlines():
        if ln.strip().startswith("pred") and "out=" in ln and "fee=" in ln:
            parts = ln.replace("=", " ").split()
            nums = [int(t) for t in parts if t.isdigit()]
            if len(nums) >= 3:
                pred_out, pred_fee = nums[0], nums[1]
            break
    n_match_event = (
        pred_out == ev_out and pred_fee == ev_fee and "MIS active vs event_end" not in out
    )
    bins = "MIS bin" in out
    if n_match_event and bins:
        return "interleave"
    if "MIS untouched bin" in out:
        return "interleave"
    model = any(
        x in out
        for x in ("MIS fee", "MIS amount_out", "MIS active", "MIS vol", "MIS idx")
    )
    if not model and bins:
        return "interleave"
    if model:
        return "model_mismatch"
    return "mismatch"


def tally(summary: list) -> dict:
    def n(status):
        return sum(1 for r in summary if r.get("status") == status)

    cleans = [r for r in summary if r.get("status") == "clean"]
    return {
        "n": len(summary),
        "clean": n("clean"),
        "interleave": n("interleave"),
        "stale_before": n("stale_before"),
        "stale_after": n("stale_after"),
        "apply_fail": n("apply_fail"),
        "model_mismatch": n("model_mismatch"),
        "gone": n("gone"),
        "fail": n("fail") + n("6001"),
        "exact": n("clean"),
        "clean_walks": [r.get("walk") for r in cleans if r.get("walk") is not None],
        "clean_multi_bin": sum(1 for r in cleans if (r.get("walk") or 0) >= 1),
    }


def write_summary(outdir: Path, summary: list) -> None:
    report = tally(summary)
    report["rows"] = summary
    (outdir / "SUMMARY.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def finish_landed(pend: dict, tx: dict, outdir: Path, cmp_bin: str, summary: list) -> None:
    sig, pair, row, before = pend["sig"], pend["pair"], pend["row"], pend["before"]
    if tx.get("meta", {}).get("err") is not None:
        status = "6001" if is_6001(tx) else "fail"
        print(f"  {sig[:8]} {status}", flush=True)
        summary.append({"sig": sig, "status": status})
        write_summary(outdir, summary)
        return
    try:
        after = d.pricing_snap(pair, K)
    except Exception as e:
        print(f"  {sig[:8]} after-snap fail {e}", flush=True)
        return
    if after is None:
        print(f"  {sig[:8]} after-snap empty", flush=True)
        return
    evs = ev.events_from_tx(tx)
    event = next((e for e in evs if e.get("kind") == "Swap2Evt"), None) or (
        evs[0] if evs else None
    )
    if event is None:
        print(f"  {sig[:8]} no Swap2Evt", flush=True)
        summary.append({"sig": sig, "status": "no_event"})
        write_summary(outdir, summary)
        return
    start = int(event["start_bin_id"])
    stale = before["lb"]["active_id"] != start
    rec = {
        "sig": sig,
        "pair": pair,
        "n_ain": int(row["n_ain"]),
        "n_dir": int(row["n_dir"]),
        "min_out": int(row.get("min_out") or 0),
        "shred_slot": row.get("slot"),
        "tx_slot": tx.get("slot"),
        "block_time": tx.get("blockTime"),
        "before": snap_json(before),
        "after": snap_json(after),
        "event": event,
        "stale_before": stale,
    }
    n = len(list(outdir.glob("triple_*.json")))
    stem = f"triple_{n:04d}_{sig[:8]}"
    (outdir / f"{stem}.json").write_text(json.dumps(rec, indent=2) + "\n", encoding="utf-8")
    rec["walk"] = walk_len(event)
    if stale:
        print(
            f"  {sig[:8]} STALE_BEFORE snap_active={before['lb']['active_id']} "
            f"event {start}->{event['end_bin_id']} after={after['lb']['active_id']}",
            flush=True,
        )
        summary.append(
            {
                "sig": sig,
                "status": "stale_before",
                "event": event,
                "file": stem,
                "walk": rec["walk"],
            }
        )
        write_summary(outdir, summary)
        return
    tri = outdir / f"{stem}.tri"
    pack_tri(
        tri,
        {
            "n_dir": rec["n_dir"],
            "n_ain": rec["n_ain"],
            "min_out": rec["min_out"],
            "block_time": rec["block_time"],
        },
        before,
        after,
        event,
    )
    print(
        f"  {sig[:8]} triple {stem} {start}->{event['end_bin_id']} "
        f"fee={event['fee']} out={event['amount_out']}",
        flush=True,
    )
    cmp = {
        "sig": sig,
        "status": "mismatch",
        "file": stem,
        "walk": rec["walk"],
        "event": {
            "start_bin_id": event["start_bin_id"],
            "end_bin_id": event["end_bin_id"],
            "fee": event.get("fee"),
            "amount_out": event.get("amount_out"),
        },
    }
    if cmp_bin:
        try:
            p = subprocess.run(
                [cmp_bin, str(tri)],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            print(p.stdout, end="", flush=True)
            if p.stderr:
                print(p.stderr, end="", flush=True)
            cmp["cmp_rc"] = p.returncode
            cmp["cmp_out"] = p.stdout
            cmp["status"] = classify_compare(p.stdout, p.returncode, rec["after"], event)
            cmp["exact"] = cmp["status"] == "clean"
        except Exception as e:
            cmp["exact"] = False
            cmp["cmp_err"] = str(e)
            cmp["status"] = "mismatch"
            print(f"  {sig[:8]} cmp fail {e}", flush=True)
    summary.append(cmp)
    write_summary(outdir, summary)
    t = tally(summary)
    print(
        f"  {sig[:8]} class={cmp.get('status')} walk={rec['walk']}  "
        f"clean={t['clean']} interleave={t['interleave']} "
        f"stale_b={t['stale_before']} stale_a={t['stale_after']} "
        f"multi={t['clean_multi_bin']}",
        flush=True,
    )


def main() -> int:
    live.load_dotenv()
    seen = Path(sys.argv[1] if len(sys.argv) > 1 else "seen_n.jsonl")
    outdir = Path(sys.argv[2] if len(sys.argv) > 2 else "state005")
    cmp_bin = sys.argv[3] if len(sys.argv) > 3 else ""
    outdir.mkdir(parents=True, exist_ok=True)
    seen.touch(exist_ok=True)
    summary: list = []
    prev = outdir / "SUMMARY.json"
    if prev.exists():
        try:
            summary = list((json.loads(prev.read_text()) or {}).get("rows") or [])
        except Exception:
            summary = []
    pending: list[dict] = []
    print(f"STATE-005  capture  seen={seen} out={outdir} cmp={cmp_bin}", flush=True)
    print("  snap-first then poll land  reserve=bin-sum  NO SEND", flush=True)
    print(f"  loaded {len(summary)} prior rows  {tally(summary)}", flush=True)
    with seen.open("r", encoding="utf-8") as f:
        f.seek(0, os.SEEK_END)
        while True:
            while True:
                line = f.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except Exception:
                    continue
                pool_hex, sig_hex = row.get("pool"), row.get("sig")
                if not pool_hex or not sig_hex:
                    continue
                pair = d._pk(bytes.fromhex(pool_hex))
                sig = b58e(bytes.fromhex(sig_hex))
                print(
                    f"N  {sig[:8]}  {pair[:8]}  ain={row.get('n_ain')} dir={row.get('n_dir')}",
                    flush=True,
                )
                try:
                    before = d.pricing_snap(pair, K)
                except Exception as e:
                    print(f"  {sig[:8]} before-snap fail {e}", flush=True)
                    continue
                if before is None:
                    print(f"  {sig[:8]} before-snap empty", flush=True)
                    continue
                print(
                    f"  {sig[:8]} before slot={before['slot']} "
                    f"active={before['lb']['active_id']} "
                    f"vol={before['lb']['vol_acc']}/{before['lb']['vol_ref']} "
                    f"bins={len(before['bins'])}",
                    flush=True,
                )
                pending.append(
                    {
                        "row": row,
                        "pair": pair,
                        "sig": sig,
                        "before": before,
                        "t0": time.time(),
                    }
                )
            still = []
            for pend in pending:
                if time.time() - pend["t0"] > 45.0:
                    print(f"  {pend['sig'][:8]} GONE", flush=True)
                    summary.append({"sig": pend["sig"], "status": "gone"})
                    write_summary(outdir, summary)
                    continue
                try:
                    tx = fetch_tx(pend["sig"])
                except Exception:
                    still.append(pend)
                    continue
                if not tx:
                    still.append(pend)
                    continue
                try:
                    finish_landed(pend, tx, outdir, cmp_bin, summary)
                except Exception as e:
                    print(f"  {pend['sig'][:8]} finish err {e}", flush=True)
            pending = still
            time.sleep(0.15)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
