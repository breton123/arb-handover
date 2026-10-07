#!/usr/bin/env python3
"""EXEC-LIVE-003 — observe successful mainnet Pump Sell account vectors.

No send. No deploy. Writes arb-cap/exec_live003/PUMP24.json.
"""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "arb-cap"))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
FEE_PROG = "pfeeUxB6jkeY1Hxd7CsFCAjcbHA9rWtchMGdZ6VojVZ"
PAIR = "BRjMA8UALNp3diTKHfdeAn5riQrYPoaYez7KhYWD4oEs"
TOKENKEG = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKEN2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
SOL = live.SOL
SELL = bytes.fromhex("33e685a4017f83ad")
OUT = Path(__file__).resolve().parents[2] / "arb-cap" / "exec_live003"
OUT_REMOTE = Path("/home/louis/arb-cap/exec_live003")


def ix_accounts(ix: dict, keys: list[str]) -> list[str]:
    out = []
    for a in ix.get("accounts") or []:
        out.append(keys[a] if isinstance(a, int) else a)
    return out


def ix_program(ix: dict, keys: list[str]) -> str | None:
    pid = ix.get("programId")
    if pid is not None:
        return pid
    idx = ix.get("programIdIndex")
    if idx is not None and idx < len(keys):
        return keys[idx]
    return None


def writable_set(tx: dict, keys: list[str]) -> set[str]:
    msg = tx["transaction"]["message"]
    header = msg.get("header") or {}
    n = len(msg.get("accountKeys") or [])
    n_ro_signed = int(header.get("numReadonlySignedAccounts") or 0)
    n_ro_unsigned = int(header.get("numReadonlyUnsignedAccounts") or 0)
    n_signed = int(header.get("numRequiredSignatures") or 0)
    wr: set[str] = set()
    for i, k in enumerate(keys[:n]):
        signed = i < n_signed
        if signed and i < n_signed - n_ro_signed:
            wr.add(k if isinstance(k, str) else k.get("pubkey", k))
        if not signed and i < n - n_ro_unsigned:
            wr.add(k if isinstance(k, str) else k.get("pubkey", k))
    loaded = (tx.get("meta") or {}).get("loadedAddresses") or {}
    wr.update(loaded.get("writable") or [])
    return wr


def iter_ixs(tx: dict) -> list[dict]:
    ixs = list(tx["transaction"]["message"].get("instructions") or [])
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        ixs.extend(g.get("instructions") or [])
    return ixs


def sells_from_sigs(addr: str, limit: int) -> list[dict]:
    found = []
    sigs = d.rpc("getSignaturesForAddress", [addr, {"limit": limit}]) or []
    for ent in sigs:
        if ent.get("err"):
            continue
        tx = d.rpc("getTransaction", [
            ent["signature"],
            {"encoding": "json", "maxSupportedTransactionVersion": 1},
        ])
        if not tx or (tx.get("meta") or {}).get("err"):
            continue
        keys = d.tx_keys(tx)
        wr = writable_set(tx, keys)
        for ix in iter_ixs(tx):
            if ix_program(ix, keys) != PUMP:
                continue
            raw = ix.get("data")
            data = d._b58_any(raw) if isinstance(raw, str) else bytes(raw or [])
            if data[:8] != SELL:
                continue
            accs = ix_accounts(ix, keys)
            if len(accs) < 22:
                continue
            found.append({
                "sig": ent["signature"],
                "slot": ent.get("slot"),
                "n": len(accs),
                "accs": accs,
                "writable": [a in wr for a in accs],
                "src": addr[:8],
            })
            break
        if len(found) >= 8:
            break
    return found


def inspect(pks: list[str]) -> list[dict]:
    accs = d.get_multiple(pks)
    rows = []
    for pk, a in zip(pks, accs):
        owner = (a or {}).get("owner")
        raw = (a or {}).get("data") or b""
        mint = d._pk(raw[0:32]) if len(raw) >= 72 and owner in (TOKENKEG, TOKEN2022) else None
        rows.append({
            "pk": pk,
            "exists": bool(a),
            "owner": owner,
            "dlen": len(raw) if a else 0,
            "mint": mint,
            "token_prog": owner if owner in (TOKENKEG, TOKEN2022) else None,
        })
    return rows


def try_derive(base_mint: str, quote_mint: str, fee_rec: str, fee_ata: str) -> dict:
    guesses = []
    for seeds, name in (
        ([b"fee_recipient"], "fee_recipient"),
        ([b"fee-recipient"], "fee-recipient"),
        ([b"buyback"], "buyback"),
        ([b"buyback_fee"], "buyback_fee"),
        ([b"protocol_fee_recipient"], "protocol_fee_recipient"),
        ([b"pool-v2", d.b58decode(base_mint)], "pool-v2+base"),
    ):
        try:
            pk = d._pk(d.find_pda(seeds, d.b58decode(PUMP)))
        except Exception:
            continue
        guesses.append({"name": name, "pk": pk, "match_rec": pk == fee_rec})
        ata_k = None
        try:
            ata_k = d._pk(d.find_pda(
                [d.b58decode(fee_rec), d.b58decode(TOKENKEG), d.b58decode(quote_mint)],
                d.b58decode("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"),
            ))
        except Exception:
            pass
        if ata_k:
            guesses.append({"name": name + "+ata_quote", "pk": ata_k, "match_ata": ata_k == fee_ata})
    return {"pda_guesses": guesses}


def main() -> int:
    live.load_dotenv()
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        OUT_REMOTE.mkdir(parents=True, exist_ok=True)
        dest = OUT_REMOTE
    except OSError:
        dest = OUT

    rows = sells_from_sigs(PAIR, 40)
    if len(rows) < 3:
        rows.extend(sells_from_sigs(PUMP, 25))
    # Dedup by sig.
    seen = set()
    uniq = []
    for r in rows:
        if r["sig"] in seen:
            continue
        seen.add(r["sig"])
        uniq.append(r)
    if not uniq:
        print("EXEC-LIVE-003  no successful Pump Sell found")
        dest.joinpath("PUMP24.json").write_text(json.dumps({"n": 0}, indent=2) + "\n")
        return 1

    n_counts = Counter(r["n"] for r in uniq)
    print(f"EXEC-LIVE-003  sells={len(uniq)} n={dict(n_counts)}")

    # Inspect last 6 accounts of the longest / modal n.
    modal_n = n_counts.most_common(1)[0][0]
    sample = [r for r in uniq if r["n"] == modal_n][:5]
    last_pks = []
    for r in sample:
        last_pks.extend(r["accs"][-6:])
        last_pks.extend(r["accs"][19:22])
    last_pks = list(dict.fromkeys(last_pks))
    info = {row["pk"]: row for row in inspect(last_pks)}

    compared = []
    for r in sample:
        accs = r["accs"]
        tail = []
        for i in range(max(0, r["n"] - 6), r["n"]):
            pk = accs[i]
            inf = info.get(pk) or {"pk": pk}
            tail.append({
                "i": i,
                "pk": pk,
                "writable": r["writable"][i],
                **{k: inf.get(k) for k in ("exists", "owner", "dlen", "mint", "token_prog")},
            })
        compared.append({
            "sig": r["sig"],
            "n": r["n"],
            "pool": accs[0],
            "tail": tail,
        })
        print(f"  {r['sig'][:12]} n={r['n']} pool={accs[0][:8]}")
        for t in tail:
            print(
                f"    [{t['i']:02}] wr={int(t['writable'])} "
                f"own={(t.get('owner') or '')[:8]} mint={(t.get('mint') or '')[:8]} {t['pk'][:8]}"
            )

    # Consensus last-3 for modal n.
    last3 = [tuple(r["accs"][-3:]) for r in sample]
    vary = len(set(last3)) > 1
    fee_rec = sample[0]["accs"][-2] if modal_n >= 24 else sample[0]["accs"][-1]
    fee_ata = sample[0]["accs"][-1] if modal_n >= 24 else None
    if modal_n >= 24:
        fee_rec = sample[0]["accs"][-2]
        fee_ata = sample[0]["accs"][-1]
        pool_v2 = sample[0]["accs"][-3]
    elif modal_n == 23:
        pool_v2 = sample[0]["accs"][-1]
        fee_rec = None
        fee_ata = None
    else:
        pool_v2 = sample[0]["accs"][-1]
        fee_rec = None
        fee_ata = None

    base = sample[0]["accs"][3] if len(sample[0]["accs"]) > 4 else ""
    quote = sample[0]["accs"][4] if len(sample[0]["accs"]) > 5 else ""
    derive = try_derive(base, quote, fee_rec or "", fee_ata or "") if fee_rec else {}

    # Does fee_recipient vary across samples?
    recs = [r["accs"][-2] for r in sample if r["n"] >= 24]
    atas = [r["accs"][-1] for r in sample if r["n"] >= 24]
    v2s = [r["accs"][-3] for r in sample if r["n"] >= 24]

    js = {
        "sells": len(uniq),
        "n_counts": dict(n_counts),
        "modal_n": modal_n,
        "last3_varies": vary,
        "pool_v2_unique": list(dict.fromkeys(v2s)),
        "fee_recipient_unique": list(dict.fromkeys(recs)),
        "fee_recipient_quote_unique": list(dict.fromkeys(atas)),
        "base_mint": base,
        "quote_mint": quote,
        "derive": derive,
        "samples": compared,
        "note": (
            "Observed successful Pump Sell CPI account tails. "
            "fee_recipient / fee_recipient_quote taken from last two metas when n>=24."
        ),
    }
    dest.joinpath("PUMP24.json").write_text(json.dumps(js, indent=2) + "\n")
    print(f"wrote {dest / 'PUMP24.json'}  modal_n={modal_n} vary={vary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
