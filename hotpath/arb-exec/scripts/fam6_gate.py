#!/usr/bin/env python3
"""FAM-6 short gate: DLMM→DLMM mint continuity → quote already trusted → pack.

Does not send. Does not touch ONESHOT#6 / OUR_EXEC / STATE-007 / Rabbit.
Does not deploy from the funded oneshot wallet.
"""
from __future__ import annotations

import json
import os
import struct
from collections import defaultdict
from pathlib import Path

SOL = "So11111111111111111111111111111111111111112"
DISC = b"ARBDLMM2"
HURDLE = 525_000
ONESHOT_WALLET = "HHNzjTABPAbPY5uEXU7NTvfmcD9RYTdXtLntyKAtpXLX"
UNIV = Path(os.environ.get("LIVEUNIV", "/home/louis/captures/paper_orbit/liveuniv.json"))
OUT = Path("/home/louis/arb-cap/fam6/GATE.json")


def token_of(mx: str, my: str) -> str | None:
    if SOL in (mx, my) and mx != my:
        return mx if my == SOL else my
    return None


def enumerate_dlmm2(pools: list[dict]) -> list[dict]:
    by_tok: dict[str, list[int]] = defaultdict(list)
    dlmm = []
    for i, p in enumerate(pools):
        if str(p.get("proto") or p.get("kind") or "") != "dlmm":
            continue
        mx, my = p.get("mx") or p.get("mint_x"), p.get("my") or p.get("mint_y")
        tok = token_of(str(mx), str(my))
        if not tok:
            continue
        by_tok[tok].append(i)
        dlmm.append(i)
    routes = []
    for tok, idxs in by_tok.items():
        if len(idxs) < 2:
            continue
        for a in idxs:
            for b in idxs:
                if a == b:
                    continue
                pa, pb = pools[a], pools[b]
                mx_a, my_a = str(pa.get("mx") or pa.get("mint_x")), str(pa.get("my") or pa.get("mint_y"))
                mx_b, my_b = str(pb.get("mx") or pb.get("mint_x")), str(pb.get("my") or pb.get("mint_y"))
                # SOL → T on A, T → SOL on B
                if token_of(mx_a, my_a) != tok or token_of(mx_b, my_b) != tok:
                    continue
                routes.append({
                    "seq": "dlmm-dlmm",
                    "family": 6,
                    "n_hop": 2,
                    "p0": a,
                    "p1": b,
                    "pk0": pa.get("pubkey"),
                    "pk1": pb.get("pubkey"),
                    "token": tok,
                    "start": SOL,
                    "end": SOL,
                    "mint_ok": tok != SOL and mx_a != my_a and mx_b != my_b,
                    "dir0": 1,
                    "dir1": 0,
                })
    return routes


def pack_ix(amount: int, min_profit: int, dir0: int, dir1: int) -> bytes:
    return DISC + struct.pack("<QQBB", amount, min_profit, dir0, dir1)


def main() -> int:
    if not UNIV.exists():
        print("no liveuniv.json")
        return 1
    u = json.loads(UNIV.read_text(encoding="utf-8"))
    pools = u.get("pools") or []
    routes = enumerate_dlmm2(pools)
    ok = [r for r in routes if r["mint_ok"]]
    tokens = sorted({r["token"] for r in ok})
    ix_guard = pack_ix(50_000_000, 1_000_000_000, 1, 0)
    ix_real = pack_ix(50_000_000, HURDLE, 1, 0)
    assert len(ix_guard) == 26 and ix_guard[:8] == DISC
    assert ix_guard[16:24] == struct.pack("<Q", 1_000_000_000)

    report = {
        "family": 6,
        "seq": "dlmm-dlmm",
        "pools_dlmm_sol": len({r["p0"] for r in ok} | {r["p1"] for r in ok}),
        "routes": len(ok),
        "tokens": len(tokens),
        "mint_continuity": "PASS" if ok else "FAIL",
        "ix_len": len(ix_guard),
        "disc": "ARBDLMM2",
        "custom6_min_profit": 1_000_000_000,
        "real_hurdle": HURDLE,
        "our_exec_untouched": "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K",
        "deploy_blocked_wallet": ONESHOT_WALLET,
        "simulate": "SKIP_NO_PROGRAM" if not os.environ.get("FAM6_PROGRAM") else "SET",
        "oneshot6": "untouched",
        "sample": ok[:8],
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(
        f"FAM6  routes={len(ok)} tokens={len(tokens)} "
        f"mint={report['mint_continuity']} ix={len(ix_guard)} "
        f"guard={1_000_000_000} hurdle={HURDLE}",
        flush=True,
    )
    print(f"  program=arb-exec/program_dlmm2  disc=ARBDLMM2  OUR_EXEC=frozen", flush=True)
    print(f"  deploy from {ONESHOT_WALLET} is forbidden while #6 is armed", flush=True)
    if os.environ.get("FAM6_PROGRAM"):
        print("  FAM6_PROGRAM set — simulate is a later control-plane step", flush=True)
    else:
        print("  next: deploy program_dlmm2 to a NEW id, then FAM6_PROGRAM=… simulate Custom(6)", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
