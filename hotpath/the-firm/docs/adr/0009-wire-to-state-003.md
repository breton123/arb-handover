# WIRE-TO-STATE-003 Reed-Solomon FEC

## Context

WIRE-TO-STATE-002 showed 376k fec-possible and 0 recovered. Live Pump
tx_complete is blocked on erasure coding, not the state engine.

## Decision

Recover a FEC set as soon as n shards exist. Authenticate by rebuilding
the chained Merkle tree and comparing the root implied by received proofs.
Same signature on every shard. First-writer wins. No whole-slot wait.
No Ed25519 leader check (no schedule). DLMM / AUTH / Pump math untouched.

## Alternatives Considered

- Firedancer SIMD RS: faster, large import. Start with Vandermonde invert.
- Skip Merkle: rejected; recovered bytes must match the signed root.

## Consequences

`fec_auth_fail` freezes the set. RS CPU is reported separately from
capture-TSC first_rx → tx_complete.
