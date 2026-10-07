# WIRE-TO-STATE-002 real OrbitFlare reconstruction

## Context

WIRE-TO-STATE-001 showed shred payload → exact publish is sub-2 µs once a
complete authenticated Pump tx and certified prestate exist. Live traffic
arrives as fragmented OrbitFlare shreds, sometimes FEC-coded. Historical
boxes still have FEEDCAP1; this tree records FIRMCAP1.

## Decision

Add a slot-local assembler that concatenates contiguous data shreds ending
at DATA_COMPLETE, plus a cap reader for FIRMCAP1 and FEEDCAP1. Replay uses
the same `wire_to_state` framed path. Reed-Solomon is counted (`fec-possible`)
but not implemented. DLMM stays blocked.

## Alternatives Considered

- Claim FAST_CUSTOM without AUTH: rejected (false exact).
- Implement RS in this slice: rejected until concat + completeness are
  measured on real caps.
- Copy 400G of captures locally: rejected; replay on the capture host.

## Consequences

- `packet_rx → tx_complete` is the honest reconstruction clock on shred-only
  files.
- Exact publish samples appear only when AUTH is already in CompactState.
- FEC-recovered remains 0 until a dedicated recover path exists.
