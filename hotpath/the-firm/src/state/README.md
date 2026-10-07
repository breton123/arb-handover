# state engine

## Purpose

Advance compact strategy state from an authenticated, ordered transaction:

```text
Ingress (OrderedTx)
    → classify
    → dependency lookup
    → transaction-local overlay
    → FAST_CUSTOM or reject (FALLBACK / UNKNOWN)
    → atomic publish + state certificate
```

Long-term target: **sub-1 ms** warm `apply_tx()`. This is not a validator
and not a trading strategy. Measured classify+overlay+publish is not
shred → reconstruct → publish.

## Responsibilities

- Normalized transaction input (legacy / v0 / v1 metadata)
- Compact pools, token accounts, system lamports
- Write-set classification and residency lookup
- Overlay + rollback
- Atomic publication with `state_cert_t`
- Offline oracle compare

## Non-responsibilities

- Packet RX, shred parse, FEC, reconstruction
- Quoting or sending transactions
- RPC on the hot path
- Full SVM / Firedancer runtime
- DLMM bins (FALLBACK)
- Token-2022 extensions (UNKNOWN)

## Invariants

- Canonical rows are not written until the whole transaction succeeds.
- Failed / unknown relevant writes are not identity.
- Repeated pool/token visits read the provisional overlay row.
- Publication is all-or-nothing per transaction.
- Certificate records slot, tx_index, signature, base_generation,
  model_version, program_hash, exactness_class, dependency_hash.

## Pump

Supported swap shapes: `sell`, `buy_exact_quote_in`, exact-out `buy`.
Exact-out inverts CORE-006 (PUMP-TX-016). Corpus: 10 000 exact-in
vectors plus exact-out invert and sell+buy_out sequences.

## Token / ATA / System (minimal)

transfer, SyncNative, ATA create/init, close, system transfer (WSOL
fund). Pump legs may update user + vault token rows in the same overlay.

## Testing

```text
make test
./build/test_pump_oracle
./build/test_pump_oracle002
./build/state_bench
./build/state_pipeline_bench
```

Pump admission: `docs/oracle/PUMP-ORACLE-001.md` then corpus-scale
`docs/oracle/PUMP-ORACLE-002.md` (4118 usable, 0 UNEXPLAINED).
PUMPSTATE-001 (framed OF tx → overlay): `docs/wire/PUMPSTATE-001.md`.
AUTHSTATE-001 (snapshot seed + coherent AUTH compare):
`docs/wire/AUTHSTATE-001.md`.
ANCHOR-001 (finalized Shyft snapshot, per-pool `anchor_slot`, OF-first
catch-up): `docs/wire/ANCHOR-001.md`.
LIVE-STATE-001 (shadow daemon, AUTH referee): `docs/wire/LIVE-STATE-001.md`.
LUT-001 (sparse cache + getAccountInfo learner): `docs/wire/LUT-001.md`.
WIRE-TO-STATE-001: `docs/wire/WIRE-TO-STATE-001.md`.
WIRE-TO-STATE-002: `docs/wire/WIRE-TO-STATE-002.md`. DLMM still waits.

## Performance

`state_bench` warm p50 **370 ns**, p99 **1.05 µs**. Staged pipeline
p50 **530 ns** (`state_pipeline_bench`). See `docs/benchmarks/state.md`.
Sanitizer builds are not latency.
