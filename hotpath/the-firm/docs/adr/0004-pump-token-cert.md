# Pump exact-out, token overlay, publication certificates

## Context

v0 covered exact-in Pump only. Real Pump txs also use exact-out `buy`,
and pool vaults are token accounts. Router paths wrap/unwrap SOL and
create ATAs. Publication needed an explicit exactness record.

## Decision

1. Exact-out buy inverts CORE-006 exact-in (PUMP-TX-016 binary search).
2. CompactState gains token and system rows; the overlay commits them
   atomically with pool rows.
3. Minimal Token/ATA/System: transfer, SyncNative, ATA create/init,
   close, system transfer (WSOL funding). Anything else fail-closed.
4. Token-2022 `extensions_mask != 0` is `UNKNOWN`.
5. Every successful publish writes `state_cert_t` (slot, tx_index, sig,
   base_generation, model_version, program_hash, exactness_class,
   dependency_hash).
6. Oracle replay compares our successor to a certified post-state.
   Agave/Firedancer are not invoked on the hot path.
7. DLMM remains `FALLBACK`.

## Alternatives Considered

- Closed-form exact-out: rejected; fee ceil/clamp is not cleanly invertible.
- Mutate vaults without token rows: rejected; Pump/router semantics
  depend on tx-local balances.
- Embed a runtime for oracle: rejected for this slice; fixtures carry
  certified post-state.

## Consequences

- `IX_KIND_PUMP_BUY` is `FAST_CUSTOM`.
- `apply_tx` does classify → lookup → overlay → publish.
- Pipeline latency is measured per stage in `state_pipeline_bench`.
