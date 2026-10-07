# AUTHSTATE-001 / PUMPBOOT-001

Trusted Pump snapshot → CompactState seed → PUMPSTATE-001 overlay →
compare predicted terminal Pump fields to **later AUTH writes for the
same committed transaction**.

```text
trusted snapshot jsonl
        ↓
   compact_pool_arm
        ↓
 framed OF tx (PUMPSTATE-001)
        ↓
 predicted terminal pump_state_t
        ↓
 AUTH writes (slot, signature, account)
        ↓
 EXACT / MISMATCH / INCOMPLETE
```

This is not a generic Solana runtime and not the old account-state
service. Overlay and publish stay where they are.

## Snapshot (`--pools`)

One jsonl object per armed pool. Compact fields only:

```text
pool_hex, vault_base_hex, vault_quote_hex, slot,
reserve_base, reserve_quote, virtual_quote,
lp_fee_bps, protocol_fee_bps, creator_fee_bps, disabled
```

`Y_effective = quote_vault + virtual_quote` remains inside `pump.c`.
Do not omit `virtual_quote` when the snapshot has a nonzero value.

Armed pools are `POOL_GRADE_EXACT` / `POOL_AUTH_READY` with a per-pool
`anchor_slot` (the finalized snapshot fetch `context.slot`) and vault identities.

## AUTH writes (`--auth`)

One jsonl object per account write, tagged with the **transaction
signature** that produced it:

```text
sig_hex, pubkey_hex, slot, write_version, role,
amount                  (vault_base / vault_quote)
reserve_* / virtual_quote / *_fee_bps   (pool)
```

A prediction is judged only when the required set for that signature
is present (pool + vaults when those pubkeys were seeded). Writes from
another signature are ignored. Pool reserves that disagree with vault
amounts in the same vector are **mixed** → `AUTH_INCOMPLETE`, not
truth.

## Tests

```text
./build/test_authstate
./build/authstate_001 --self-test
./build/authstate_001 --cap FILE --pools seed.jsonl --luts warm.jsonl --auth writes.jsonl
```

Fixture seed: `tests/fixtures/authstate/seed.jsonl`.

Live seed from the Shyft adapter (buffer OF first):

```text
python tools/anchor/snapshot.py --provider shyft --pools pools.txt --out seed.jsonl
./build/authstate_001 --cap FILE --pools seed.jsonl --luts configs/luts/warm.jsonl
```

Without `--auth`, emitted predictions judge `AUTH_INCOMPLETE`. That is not a match claim.

Live composition (shadow daemon): `docs/wire/LIVE-STATE-001.md`.

## Scoreboard

Pump txs seen, LUT-resolved, supported transitions, exact prestate,
predictions, terminal exact, mismatch, DIRTY reasons (`LUT_MISS`,
`UNKNOWN_PUMP_DISC`, `STATE_GAP`, `APPLY_FAIL`, `AUTH_INCOMPLETE`,
`AUTH_MIXED_VECTOR`).

For every prediction labelled `AUTH_EXACT`, predicted
`reserve_base` / `reserve_quote` / `virtual_quote` (and fees when
present on the pool write) must equal the assembled AUTH vector.
