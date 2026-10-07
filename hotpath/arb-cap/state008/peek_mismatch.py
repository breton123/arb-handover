#!/usr/bin/env python3
import json
from pathlib import Path

SIG = "81ec9fa0bb7be14fd7f5ffa8156ecb043ecd188cbb6bc50ec7e0feade52614dd63b8c907a9462ce4bf3a2e7fb16d41320d7a2272070ac7d527c6466ff3bb2805"
audit = Path("/home/louis/captures/paper_orbit/opp_synced.jsonl")
univ = json.loads(Path("/home/louis/captures/paper_orbit/liveuniv.json").read_text())
pools = univ.get("pools") or []
p26 = next((p for p in pools if int(p.get("idx") or -1) == 26), None)
print("POOL26", json.dumps({k: p26.get(k) for k in ("idx", "proto", "pubkey", "mx", "my") if p26}, indent=2) if p26 else None)
print("METRICS", Path("/home/louis/captures/state008/METRICS.json").read_text()[:2500])
print("SHADOW", Path("/home/louis/captures/state008/SHADOW.jsonl").read_text())
hits = 0
if audit.exists():
    with audit.open("r", encoding="utf-8") as f:
        for line in f:
            if SIG[:16] not in line and '"pool_idx":26' not in line and '"pool_idx": 26' not in line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if rec.get("kind") != "opp_synced":
                continue
            if rec.get("sig_hex") == SIG or (rec.get("n") or {}).get("pool_idx") == 26:
                hits += 1
                if hits <= 3 or rec.get("sig_hex") == SIG:
                    n = rec.get("n") or {}
                    sp = rec.get("s_prime") or {}
                    print("OPP", json.dumps({
                        "sig": rec.get("sig_hex"),
                        "auth_slot": rec.get("auth_slot"),
                        "state_version": rec.get("state_version_before"),
                        "n": n,
                        "s_prime_keys": list(sp) if isinstance(sp, dict) else sp,
                        "active_id": sp.get("active_id") if isinstance(sp, dict) else None,
                        "active_after": sp.get("active_after") if isinstance(sp, dict) else None,
                        "vol_acc": sp.get("vol_acc") if isinstance(sp, dict) else None,
                        "touched": (sp.get("touched") or [])[:6] if isinstance(sp, dict) else None,
                    }))
print("hits", hits)
