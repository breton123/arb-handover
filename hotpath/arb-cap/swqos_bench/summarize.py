#!/usr/bin/env python3
import collections
import json
from pathlib import Path

d = Path("/home/louis/arb-cap/swqos_bench")


def pct(xs, q):
    if not xs:
        return 0
    i = int(q * (len(xs) - 1) + 0.5)
    i = min(max(i, 0), len(xs) - 1)
    return xs[i]


for name in ("warm", "cold"):
    rows = [json.loads(l) for l in (d / f"{name}.jsonl").read_text().splitlines() if l.strip()]
    print("====", name, len(rows))
    print("err", collections.Counter(r["err"] for r in rows))
    print("conn", collections.Counter(r.get("conn") for r in rows))
    ok = [r for r in rows if r["err"] != "send_fail" and r.get("send_ns", 0) > 0]
    print("ok_send", len(ok), "landed", sum(1 for r in ok if r["landed"]))
    for label, pred in (
        ("sign_all", lambda r: r.get("sign_ns", 0) > 0),
        ("send_ok", lambda r: r["err"] != "send_fail" and r.get("send_ns", 0) > 0),
        ("s2s_ok", lambda r: r["err"] != "send_fail" and r.get("sign_to_send_ns", 0) > 0),
        ("seen", lambda r: r.get("t3_ns", 0) > r.get("t2_ns", 0)),
    ):
        key = {
            "sign_all": "sign_ns",
            "send_ok": "send_ns",
            "s2s_ok": "sign_to_send_ns",
            "seen": "send_to_seen_ns",
        }[label]
        xs = sorted(r[key] for r in rows if pred(r) and r.get(key, 0) > 0)
        if not xs:
            print(label, "n=0")
            continue
        print(
            f"{label} n={len(xs)} p50={pct(xs,0.5)/1e3:.1f}us "
            f"p90={pct(xs,0.9)/1e3:.1f}us p99={pct(xs,0.99)/1e3:.1f}us "
            f"min={xs[0]/1e3:.1f} max={xs[-1]/1e3:.1f}"
        )
        if label == "send_ok":
            for us in (100, 250, 500, 1000, 2000, 5000, 10000):
                n = sum(1 for x in xs if x <= us * 1000)
                print(f"  send <= {us}us: {n}/{len(xs)}")
    fail = sorted(r["send_ns"] for r in rows if r["err"] == "send_fail")
    if fail:
        print(
            "fail_send n",
            len(fail),
            "p50_us",
            fail[len(fail) // 2] / 1e3,
            "min",
            fail[0] / 1e3,
            "max",
            fail[-1] / 1e3,
        )
    print("slots", [r["slot"] for r in rows if r.get("landed") and r.get("slot")])
    print("fees", sum(r.get("fee_lamports") or 0 for r in rows))
    seen = [r for r in rows if r.get("t3_ns", 0) > r.get("t2_ns", 0)]
    if seen:
        xs = sorted(r["sign_to_seen_ns"] for r in seen)
        print(
            f"sign_to_seen n={len(xs)} p50={pct(xs,0.5)/1e6:.2f}ms "
            f"p90={pct(xs,0.9)/1e6:.2f}ms min={xs[0]/1e6:.2f} max={xs[-1]/1e6:.2f}"
        )
