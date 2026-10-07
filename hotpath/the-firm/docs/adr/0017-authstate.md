# AUTHSTATE-001 / PUMPBOOT-001: seed + coherent AUTH vector

## Context

PUMPSTATE-001 can overlay a framed Pump tx only when CompactState
already holds certified prestate. Shreds cannot invent that prestate.
Later account notifications are the verification source, but mixed
account generations (new pool + old vault) are not a truth vector.

## Decision

1. Seed only the compact Pump fields the overlay uses, plus vault
   pubkeys, per-pool `anchor_slot` (finalized RPC `context.slot`), and `incarnation`
   jsonl is decoded compact state, not a generic accounts DB.
2. After a successful overlay, record a prediction keyed by
   `(slot, signature, pool)`.
3. Stage AUTH writes by the same triple. Judge only when every
   required account for that signature has arrived at that slot.
   Do not fill a missing vault from another signature or from
   mutable latest state.
4. If pool-account reserves and vault amounts disagree inside the
   same vector, treat as AUTH_INCOMPLETE (mixed), not EXACT.

## Alternatives Considered

- Rebuilding the old account-state service: rejected.
- Comparing quotes only: rejected; terminal pool/vault fields.
- Parsing live AMM account layouts in C without a frozen spec:
  rejected; snapshot/AUTH jsonl carries the compact fields.

## Consequences

- Live exact % stays incomplete until a trusted `--pools` snapshot
  and transaction-tagged `--auth` writes exist.
- Mismatches are gold-fixture candidates, not silent dirties.
