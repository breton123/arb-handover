# WIRE-TO-STATE-001 staged shred→publish

## Context

PUMP-ORACLE-002 proved 0 false exact claims. The transition kernel is
sub-µs. We did not know packet → certified publish, or which stage
dominates.

## Decision

1. Reconstruction owns wire parse (`ingress_decode_tx`) and
   DATA_COMPLETE first-tx extract (`shred_first_tx`). No FEC yet.
2. ALT lookups are FALLBACK, not guessed keys.
3. Exact publish requires `POOL_AUTH_READY` (V + fees + vaults + mint).
   Incomplete AUTH is FALLBACK and does not publish.
4. `wire_to_state` times packet_rx → tx_complete → OrderedTx → resolve
   → classify → prestate → apply → cert → publish.
5. DLMM stays blocked.

## Alternatives Considered

- FEC reconstruct now: rejected; correctness not proven.
- Publish on incomplete AUTH: rejected; oracle already forbids this.
- Measure only apply_tx: rejected; hides reconstruction/prestate.

## Consequences

- `test_wire` + `wire_to_state_bench` are the gate for a number.
- Live OrbitFlare caps can be added later without changing admission.
