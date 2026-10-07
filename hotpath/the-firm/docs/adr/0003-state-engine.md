# Compact state engine and Pump v0 overlay

## Context

Ingest and shred race deliver packets. Strategies need a successor state
after an authenticated transaction, not a validator. The old tree had a
frozen Pump exact-in kernel (`pump_apply_swap`) and a transaction-local
overlay (`tx_overlay`) that fail-closed on unknown relevant CPIs.

## Decision

1. Introduce `OrderedTx` as the state-engine input. Reconstruction, not
   `apply_tx`, owns wire parsing.
2. Keep Pump math in `src/protocols/pump` (CORE-006 bit-identical).
3. Apply through a transaction-local overlay; publish atomically or not
   at all. Exact-out Pump `buy` and unknown relevant writes are `UNKNOWN`.
4. DLMM / Token / runtime replay are `FALLBACK` or TODO — reject in v0.
5. Oracle is a compare interface only; no RPC on the hot path.

## Alternatives Considered

- Mutate canonical rows per CPI: rejected; violates atomic tx and
  rollback-on-failure.
- Hash-payload or invent swap amounts for unknown CPIs: rejected;
  unknown cannot become identity.
- Embed a full SVM: rejected; out of scope and too slow for the first cut.

## Consequences

- Bit-exact Pump successor on the official 10k vectors is the v0 gate.
- Adding DLMM does not change `apply_tx`'s overlay/publish contract.
- Wire decode and ALT resolution stay in reconstruction.
