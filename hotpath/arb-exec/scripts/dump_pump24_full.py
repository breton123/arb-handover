#!/usr/bin/env python3
"""Dump full Pump Sell CPI vectors + owners + PDA matches. No send."""
from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

_CAP = Path("/home/louis/arb-cap")
if not _CAP.is_dir():
    _CAP = Path(__file__).resolve().parents[2] / "arb-cap"
sys.path.insert(0, str(_CAP))
import live001 as live  # noqa: E402
import record_dlmm as d  # noqa: E402

PUMP = "pAMMBay6oceH9fJKBRHGP5D4bD4sWpmSwMn52FMfXEA"
FEE_PROG = "pfeeUxB6jkeY1Hxd7CsFCAjcbHA9rWtchMGdZ6VojVZ"
PAIR = "BRjMA8UALNp3diTKHfdeAn5riQrYPoaYez7KhYWD4oEs"
TOKENKEG = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
TOKEN2022 = "TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb"
ATA = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
SOL = live.SOL
SELL = bytes.fromhex("33e685a4017f83ad")
OUT = Path("/home/louis/arb-cap/exec_live003/PUMP24_FULL.json")

IDL = [
    "pool", "user", "global", "base_mint", "quote_mint",
    "user_base", "user_quote", "vault_b", "vault_q",
    "proto_fee", "proto_fee_ata", "base_tok", "quote_tok",
    "system", "ata", "event", "program",
    "creator_ata", "creator_auth", "fee_cfg", "fee_prog",
]


def ix_accounts(ix, keys):
    return [keys[a] if isinstance(a, int) else a for a in (ix.get("accounts") or [])]


def ix_program(ix, keys):
    pid = ix.get("programId")
    if pid is not None:
        return pid
    idx = ix.get("programIdIndex")
    if idx is not None and idx < len(keys):
        return keys[idx]
    return None


def writable_set(tx, keys):
    msg = tx["transaction"]["message"]
    header = msg.get("header") or {}
    n = len(msg.get("accountKeys") or [])
    n_ro_signed = int(header.get("numReadonlySignedAccounts") or 0)
    n_ro_unsigned = int(header.get("numReadonlyUnsignedAccounts") or 0)
    n_signed = int(header.get("numRequiredSignatures") or 0)
    wr = set()
    for i, k in enumerate(keys[:n]):
        pk = k if isinstance(k, str) else k.get("pubkey", k)
        signed = i < n_signed
        if signed and i < n_signed - n_ro_signed:
            wr.add(pk)
        if not signed and i < n - n_ro_unsigned:
            wr.add(pk)
    loaded = (tx.get("meta") or {}).get("loadedAddresses") or {}
    wr.update(loaded.get("writable") or [])
    return wr


def iter_ixs(tx):
    ixs = list(tx["transaction"]["message"].get("instructions") or [])
    for g in (tx.get("meta") or {}).get("innerInstructions") or []:
        ixs.extend(g.get("instructions") or [])
    return ixs


def find_sells(addr, limit, want):
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
            if len(accs) < 21:
                continue
            found.append({
                "sig": ent["signature"],
                "slot": ent.get("slot"),
                "n": len(accs),
                "accs": accs,
                "writable": [a in wr for a in accs],
            })
            break
        if len(found) >= want:
            break
    return found


def inspect(pks):
    accs = d.get_multiple(pks)
    rows = []
    for pk, a in zip(pks, accs):
        owner = (a or {}).get("owner")
        raw = (a or {}).get("data") or b""
        mint = None
        tok_owner = None
        if a and owner in (TOKENKEG, TOKEN2022) and len(raw) >= 72:
            mint = d._pk(raw[0:32])
            tok_owner = d._pk(raw[32:64])
        rows.append({
            "pk": pk,
            "exists": bool(a),
            "owner": owner,
            "dlen": len(raw) if a else 0,
            "mint": mint,
            "token_owner": tok_owner,
            "token_prog": owner if owner in (TOKENKEG, TOKEN2022) else None,
        })
    return rows


def pda(seeds, program):
    return d._pk(d.find_pda(seeds, d.b58decode(program)))


def ata(owner, mint, tok):
    return pda([d.b58decode(owner), d.b58decode(tok), d.b58decode(mint)], ATA)


def derive_all(base, quote, extras):
    guesses = []
    seed_sets = [
        (PUMP, [b"pool-v2", d.b58decode(base)], "pool-v2+base"),
        (PUMP, [b"pool-v2", d.b58decode(quote)], "pool-v2+quote"),
        (PUMP, [b"pool_v2", d.b58decode(base)], "pool_v2+base"),
        (PUMP, [b"fee_recipient"], "fee_recipient"),
        (PUMP, [b"fee-recipient"], "fee-recipient"),
        (PUMP, [b"buyback"], "buyback"),
        (PUMP, [b"buyback_fee"], "buyback_fee"),
        (PUMP, [b"buyback-fee"], "buyback-fee"),
        (PUMP, [b"buyback_recipient"], "buyback_recipient"),
        (FEE_PROG, [b"fee_recipient"], "pfee/fee_recipient"),
        (FEE_PROG, [b"fee-recipient"], "pfee/fee-recipient"),
        (FEE_PROG, [b"buyback"], "pfee/buyback"),
        (FEE_PROG, [b"buyback_fee"], "pfee/buyback_fee"),
        (FEE_PROG, [b"pool-v2", d.b58decode(base)], "pfee/pool-v2+base"),
        (FEE_PROG, [b"fee_config"], "pfee/fee_config"),
    ]
    extra_set = set(extras)
    for prog, seeds, name in seed_sets:
        try:
            pk = pda(seeds, prog)
        except Exception:
            continue
        guesses.append({"name": name, "pk": pk, "hit": pk in extra_set})
    return guesses


def main():
    live.load_dotenv()
    rows = find_sells(PAIR, 50, 4)
    rows.extend(find_sells(PUMP, 40, 12))
    seen, uniq = set(), []
    for r in rows:
        if r["sig"] in seen:
            continue
        seen.add(r["sig"])
        uniq.append(r)
    print(f"sells={len(uniq)} n={dict(Counter(r['n'] for r in uniq))}")

    # Prefer a mix of n=23 and n=24
    pick = []
    for want in (24, 23, 22, 21):
        for r in uniq:
            if r["n"] == want and r not in pick:
                pick.append(r)
            if sum(1 for x in pick if x["n"] == want) >= 3:
                break
    if not pick:
        pick = uniq[:6]

    all_pks = []
    for r in pick:
        all_pks.extend(r["accs"])
    info = {row["pk"]: row for row in inspect(list(dict.fromkeys(all_pks)))}

    samples = []
    for r in pick:
        accs = r["accs"]
        base = accs[3]
        quote = accs[4]
        extras = accs[21:]
        rows_out = []
        print(f"\n{r['sig'][:16]} n={r['n']} pool={accs[0][:8]} base={base[:8]}")
        for i, pk in enumerate(accs):
            inf = info.get(pk) or {}
            name = IDL[i] if i < len(IDL) else f"extra{i-21}"
            print(
                f"  [{i:02}] {name:16} wr={int(r['writable'][i])} "
                f"own={(inf.get('owner') or '')[:12]:12} "
                f"mint={(inf.get('mint') or '')[:8]:8} "
                f"tokown={(inf.get('token_owner') or '')[:8]:8} {pk}"
            )
            rows_out.append({
                "i": i,
                "name": name,
                "pk": pk,
                "writable": r["writable"][i],
                **{k: inf.get(k) for k in (
                    "exists", "owner", "dlen", "mint", "token_owner", "token_prog",
                )},
            })
        guesses = derive_all(base, quote, extras)
        hits = [g for g in guesses if g["hit"]]
        print("  pda hits:", hits or "none")
        # ATA of extras[0] for quote under both token programs
        ata_hits = []
        if extras:
            for tok in (TOKENKEG, TOKEN2022):
                try:
                    qata = ata(extras[0], quote, tok)
                    bata = ata(extras[0], base, tok)
                except Exception:
                    continue
                ata_hits.append({
                    "owner": extras[0],
                    "quote_ata": qata,
                    "quote_hit": qata in extras,
                    "base_ata": bata,
                    "base_hit": bata in extras,
                    "tok": tok[:8],
                })
        print("  ata from extra0:", ata_hits)
        samples.append({
            "sig": r["sig"],
            "n": r["n"],
            "pool": accs[0],
            "base": base,
            "quote": quote,
            "accounts": rows_out,
            "pda_hits": hits,
            "ata_from_extra0": ata_hits,
        })

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({
        "n_counts": dict(Counter(r["n"] for r in uniq)),
        "samples": samples,
    }, indent=2) + "\n")
    print(f"\nwrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
