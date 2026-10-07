#!/usr/bin/env python3
"""Tomorrow: fund wallet → loader-v3 OUR_EXEC → verify → sim 619 B v0 → walk → Custom(6).

Funding is the only remaining blocker. This script is the rest.
Never BPF Loader 2. Never sendTransaction of the arb. Never prints secrets.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-cap"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
DEPLOY = ROOT / ".deploy"
OUT = REPO / "arb-cap" / "exec_live002b"
WALK = OUT / "WALK.json"

MUST_WALLET = "HHNzjTABPAbPY5uEXU7NTvfmcD9RYTdXtLntyKAtpXLX"
MUST_EXEC = "38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K"
DEAD_BPF2 = "6xfcHyCsRkqbeJhX3RVd4gfHmTcQ6UWUhcP5cfGGrWNg"
LOADER_V3 = "BPFLoaderUpgradeab1e11111111111111111111111"
LOADER_V2 = "BPFLoader2111111111111111111111111111111111"
MIN_SOL = 0.15
V0_TX_LEN = 623
SOLANA_BIN = Path.home() / ".local/share/solana/install/active_release/bin/solana"

ALPH = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58_any(s: str) -> bytes:
    n = 0
    for c in s:
        n = n * 58 + ALPH.index(c)
    pad = 0
    for c in s:
        if c == "1":
            pad += 1
        else:
            break
    h = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\x00" * pad + h


def b58_pk(raw: bytes) -> str:
    return d._pk(raw)


def load_env() -> None:
    live.load_dotenv()
    rpc_file = DEPLOY / "rpc.url"
    if rpc_file.exists():
        os.environ.setdefault("HELIUS_RPC_URL", rpc_file.read_text(encoding="utf-8").strip())
    if not os.environ.get("HELIUS_RPC_URL") and os.environ.get("RPC_URL"):
        os.environ["HELIUS_RPC_URL"] = os.environ["RPC_URL"]


def rpc_url() -> str:
    u = os.environ.get("HELIUS_RPC_URL") or os.environ.get("RPC_URL")
    if u:
        return u
    k = os.environ.get("HELIUS_API_KEY")
    if k:
        return f"https://mainnet.helius-rpc.com/?api-key={k}"
    raise SystemExit("set HELIUS_RPC_URL or HELIUS_API_KEY")


def solana_cli() -> str:
    if SOLANA_BIN.exists():
        return str(SOLANA_BIN)
    found = shutil.which("solana")
    if found:
        return found
    raise SystemExit("solana CLI not found — expected ~/.local/share/solana/install/active_release/bin/solana")


def program_so() -> Path:
    cands = [
        ROOT / "program" / "target" / "deploy" / "route0.so",
        Path.home() / "arb-exec-live" / "program" / "target" / "deploy" / "route0.so",
        ROOT / "program" / "target" / "sbf-solana-solana" / "release" / "route0.so",
        Path.home() / "arb-exec-live" / "program" / "target" / "sbf-solana-solana" / "release" / "route0.so",
    ]
    for p in cands:
        if p.exists() and p.stat().st_size > 1000:
            return p
    raise SystemExit("route0.so missing — copy from arb-exec-live or cargo-build-sbf first")


def program_v3_path() -> Path:
    p = DEPLOY / "program-v3.json"
    if not p.exists():
        raise SystemExit("missing arb-exec/.deploy/program-v3.json")
    return p


def program_v3_pk() -> str:
    raw = json.loads(program_v3_path().read_text(encoding="utf-8"))
    return b58_pk(bytes(raw[32:64]))


def wallet_bytes() -> bytes:
    wpath = DEPLOY / "wallet.json"
    if wpath.exists():
        raw = json.loads(wpath.read_text(encoding="utf-8"))
        return bytes(raw)
    secret = os.environ.get("PRIVATE_KEY", "").strip()
    if not secret:
        raise SystemExit("PRIVATE_KEY missing and .deploy/wallet.json missing")
    raw = b58_any(secret)
    if len(raw) not in (32, 64):
        raise SystemExit(f"PRIVATE_KEY length {len(raw)}")
    return raw


def wallet_pk() -> str:
    raw = wallet_bytes()
    return b58_pk(raw[32:64] if len(raw) == 64 else raw)


def write_wallet_json() -> Path:
    DEPLOY.mkdir(parents=True, exist_ok=True)
    path = DEPLOY / "wallet.json"
    raw = wallet_bytes()
    if len(raw) == 32:
        raise SystemExit("need 64-byte keypair for solana CLI wallet.json")
    path.write_text(json.dumps(list(raw)), encoding="utf-8")
    os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
    return path


def acc(pk: str) -> dict | None:
    return d.get_multiple([pk])[0]


def bal_sol(pk: str) -> float:
    b = d.rpc("getBalance", [pk])
    lamports = b["value"] if isinstance(b, dict) else b
    return lamports / 1e9


def custom_code(err) -> int | None:
    if err is None:
        return None
    if isinstance(err, dict):
        ie = err.get("InstructionError")
        if isinstance(ie, list) and len(ie) >= 2 and isinstance(ie[1], dict):
            if "Custom" in ie[1]:
                return int(ie[1]["Custom"])
        if "Custom" in err:
            return int(err["Custom"])
    s = str(err)
    if "Custom" in s:
        digits = "".join(ch if ch.isdigit() else " " for ch in s.split("Custom", 1)[1])
        parts = digits.split()
        if parts:
            return int(parts[0])
    return None


def classify(sim: dict, exec_acc: dict | None) -> dict:
    err = (sim or {}).get("err")
    cu = (sim or {}).get("cu")
    logs = (sim or {}).get("logs") or []
    code = custom_code(err)
    err_s = json.dumps(err) if not isinstance(err, str) else err

    if exec_acc is None:
        gate = "NEED_DEPLOY"
        why = "OUR_EXEC account missing (ProgramAccountNotFound until loader-v3)"
    elif exec_acc.get("owner") == LOADER_V2:
        gate = "DEAD_BPF2"
        why = "OUR_EXEC owned by BPF2 — refuse. Use 38dsYLgt only."
    elif not exec_acc.get("executable"):
        gate = "NEED_DEPLOY"
        why = "OUR_EXEC exists but not executable"
    elif err is None:
        gate = "UNEXPECTED_OK"
        why = "simulate succeeded — profit guard should have Custom(6) on tiny_in"
    elif code == 6:
        gate = "CUSTOM_6"
        why = "profit guard after both CPIs — record this CU"
    elif "ProgramAccountNotFound" in err_s:
        gate = "NEED_DEPLOY"
        why = "simulate ProgramAccountNotFound"
    elif "AccountNotFound" in err_s or "AccountNotFound" in " ".join(logs):
        gate = "MISSING_ACCOUNT"
        why = f"account missing err={err_s[:200]}"
    elif "InsufficientFunds" in err_s:
        gate = "INSUFFICIENT_FUNDS"
        why = "wallet or ATA short"
    elif "Blockhash" in err_s:
        gate = "BLOCKHASH"
        why = "resign / replaceRecentBlockhash"
    elif code is not None:
        gate = f"CUSTOM_{code}"
        why = f"on-chain Custom({code}) — not the profit guard"
    else:
        gate = "OTHER"
        why = err_s[:240]

    return {
        "gate": gate,
        "why": why,
        "custom": code,
        "cu": cu,
        "err": err,
        "accepted": bool((sim or {}).get("accepted")),
        "logs_tail": logs[-8:],
    }


def preflight() -> dict:
    load_env()
    wallet = wallet_pk()
    our = program_v3_pk()
    so = program_so()
    cli = solana_cli()
    issues = []
    funding_only = True

    if our != MUST_EXEC:
        issues.append(f"program-v3.json is {our} — must be {MUST_EXEC}")
        funding_only = False
    if our == DEAD_BPF2:
        issues.append("REFUSE: program-v3.json is the dead BPF2 key")
        funding_only = False
    if wallet != MUST_WALLET:
        issues.append(f"wallet {wallet} != {MUST_WALLET} — fund the leftover wallet")
        funding_only = False

    try:
        ver = subprocess.check_output([cli, "--version"], text=True).strip()
    except Exception as e:
        ver = None
        issues.append(f"solana --version failed: {e}")
        funding_only = False

    sol = None
    try:
        sol = bal_sol(wallet)
    except Exception as e:
        issues.append(f"getBalance failed: {e}")
        funding_only = False

    exec_acc = acc(our)
    dead = acc(DEAD_BPF2)
    funded = sol is not None and sol + 1e-12 >= MIN_SOL

    if sol is not None and not funded:
        issues.append(f"FUNDING: have {sol:.6f} SOL need ≥{MIN_SOL:.2f}")

    print("PREFLIGHT")
    print(f"  wallet     {wallet}")
    print(f"  sol        {sol if sol is not None else '?'}")
    print(f"  OUR_EXEC   {our}")
    print(f"  so         {so}  ({so.stat().st_size} B)")
    print(f"  solana     {ver}")
    print(f"  exec_acc   {None if exec_acc is None else 'owner=' + str(exec_acc.get('owner')) + ' exec=' + str(exec_acc.get('executable'))}")
    print(f"  dead_bpf2  {None if dead is None else str(dead.get('lamports')) + ' lamports locked — do not reuse'}")
    for x in issues:
        print(f"  ! {x}")

    if not issues:
        print("  READY  funding is not a blocker — run deploy")
    elif funding_only and not funded and len(issues) == 1:
        print("  FUNDING_ONLY  everything else is staged")
    else:
        print("  BLOCKED  fix issues above before deploy")

    return {
        "wallet": wallet,
        "sol": sol,
        "our_exec": our,
        "so": str(so),
        "so_bytes": so.stat().st_size,
        "solana": ver,
        "exec_onchain": None if exec_acc is None else {
            "owner": exec_acc.get("owner"),
            "executable": exec_acc.get("executable"),
            "lamports": exec_acc.get("lamports"),
        },
        "dead_bpf2_lamports": None if dead is None else dead.get("lamports"),
        "issues": issues,
        "funding_only": funding_only and not funded,
        "ready": not issues,
    }


def deploy() -> None:
    load_env()
    pf = preflight()
    if pf["our_exec"] != MUST_EXEC:
        raise SystemExit("refuse deploy: program-v3 is not 38dsYLgt")
    if pf["wallet"] != MUST_WALLET:
        raise SystemExit("refuse deploy: wallet is not HHNzjTAB")
    if pf["sol"] is None or pf["sol"] < MIN_SOL:
        raise SystemExit(f"refuse deploy: {pf['sol']} SOL < {MIN_SOL} — fund {MUST_WALLET}")
    if pf["exec_onchain"] and pf["exec_onchain"].get("owner") == LOADER_V2:
        raise SystemExit("refuse deploy: target owned by BPF2")

    kp = write_wallet_json()
    so = program_so()
    pid = program_v3_path()
    cli = solana_cli()
    url = rpc_url()
    cmd = [
        cli, "program", "deploy", str(so),
        "--program-id", str(pid),
        "--keypair", str(kp),
        "--url", url,
        "--use-rpc",
        "--max-sign-attempts", "12",
    ]
    print("DEPLOY")
    print(f"  {' '.join(x if 'api-key' not in x else '<rpc>' for x in cmd)}")
    env = os.environ.copy()
    env["PATH"] = str(SOLANA_BIN.parent) + os.pathsep + env.get("PATH", "")
    r = subprocess.run(cmd, env=env)
    if r.returncode != 0:
        raise SystemExit(f"solana program deploy exited {r.returncode}")


def verify() -> dict:
    load_env()
    our = program_v3_pk()
    if our != MUST_EXEC:
        raise SystemExit("verify: program-v3 mismatch")
    a = acc(our)
    if a is None:
        raise SystemExit("verify FAIL: OUR_EXEC still missing")
    owner = a.get("owner")
    exe = bool(a.get("executable"))
    print("VERIFY")
    print(f"  OUR_EXEC   {our}")
    print(f"  owner      {owner}")
    print(f"  executable {exe}")
    print(f"  lamports   {a.get('lamports')}")
    if owner == LOADER_V2:
        raise SystemExit("verify FAIL: BPF2 — this deploy is dead")
    if owner != LOADER_V3:
        raise SystemExit(f"verify FAIL: owner {owner} != loader-v3")
    if not exe:
        raise SystemExit("verify FAIL: not executable")
    print("  OK  loader-v3 executable")
    return {"our_exec": our, "owner": owner, "executable": exe, "lamports": a.get("lamports")}


def simulate() -> dict:
    load_env()
    script = ROOT / "scripts" / "exec_live002b.py"
    env = os.environ.copy()
    r = subprocess.run([sys.executable, str(script)], env=env)
    if r.returncode != 0:
        raise SystemExit(f"exec_live002b.py exited {r.returncode}")
    report = json.loads((OUT / "report.json").read_text(encoding="utf-8"))
    return report


def walk(report: dict | None = None) -> dict:
    if report is None:
        report = json.loads((OUT / "report.json").read_text(encoding="utf-8"))
    sim = report.get("simulate") or {}
    exec_acc = acc(report.get("our_exec") or MUST_EXEC)
    w = classify(sim, exec_acc)
    w["tx_len"] = report.get("tx_len")
    w["margin"] = report.get("margin")
    w["our_exec"] = report.get("our_exec")
    w["wallet"] = report.get("wallet")
    print("WALK")
    print(f"  tx_len  {w['tx_len']}  (want {V0_TX_LEN})")
    print(f"  gate    {w['gate']}")
    print(f"  why     {w['why']}")
    print(f"  custom  {w['custom']}")
    print(f"  cu      {w['cu']}")
    if w["gate"] == "CUSTOM_6":
        print(f"  REAL_CU {w['cu']}")
    OUT.mkdir(parents=True, exist_ok=True)
    WALK.write_text(json.dumps(w, indent=2) + "\n", encoding="utf-8")
    md = OUT / "DEPLOY.md"
    lines = [
        "# EXEC deploy walk",
        "",
        f"- gate: `{w['gate']}`",
        f"- Custom: `{w['custom']}`",
        f"- CU: `{w['cu']}`",
        f"- tx_len: `{w['tx_len']}`",
        f"- why: {w['why']}",
        "",
    ]
    md.write_text("\n".join(lines), encoding="utf-8")
    return w


def run_all() -> int:
    pf = preflight()
    if not pf["ready"]:
        if pf["funding_only"]:
            print("STOP  fund the wallet, then re-run: python arb-exec/scripts/go_deploy.py all")
            return 2
        return 1
    deploy()
    verify()
    report = simulate()
    w = walk(report)
    return 0 if w["gate"] == "CUSTOM_6" else 3


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "preflight"
    if cmd == "preflight":
        pf = preflight()
        return 0 if pf["ready"] else (2 if pf["funding_only"] else 1)
    if cmd == "deploy":
        deploy()
        return 0
    if cmd == "verify":
        verify()
        return 0
    if cmd == "simulate":
        simulate()
        return 0
    if cmd == "walk":
        walk()
        return 0
    if cmd == "all":
        return run_all()
    raise SystemExit("usage: go_deploy.py [preflight|deploy|verify|simulate|walk|all]")


if __name__ == "__main__":
    raise SystemExit(main())
