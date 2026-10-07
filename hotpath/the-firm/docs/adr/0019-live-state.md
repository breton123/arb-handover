# LIVE-STATE-001: shadow Pump daemon, AUTH as referee

## Context

PUMPSTATE, AUTHSTATE, and ANCHOR pieces existed as separate benches.
Live exactness was not composed. AUTH must not control CompactState.

## Decision

1. One shadow app (`live_state_001`) replays OF captures through
   `live_on_framed` → CompactState, then judges AUTH later.
2. Do not bind OrbitFlare UDP in this binary (`:20001` stays with the
   existing capture stack).
3. MISMATCH dirties the pool and dumps a fixture. No tolerance.
4. Score MATCH/(MATCH+MISMATCH) separately from coverage and from
   AUTH_INCOMPLETE / LUT / GAP / NO_PRESTATE.

## Alternatives Considered

- Let AUTH rewind live rows: rejected; races the edge we are measuring.
- Intra-slot processed snapshot: rejected (ANCHOR-001).
- Trading/searcher consumption of EXACT: deferred until a large zero-
  mismatch live sample exists.

## Consequences

- `authvec_t` pending tables are bounded; pred_drop is counted.
- Slot-first OF TSC is a proxy until frames carry per-tx rx time.
