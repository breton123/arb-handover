# Race winners enter the prefix assembler

## Context

Keyed first-writer-wins already existed (`shred_race_claim`). The
reconstruction path still accepted every datagram into the prefix
concat. A second feed would have duplicated data shreds at the same
index (prefix already skips repeats) but would not have produced
`% first` or pair delay stats, and would still have spent prefix CPU
on losers.

Transaction framing is now correct. The missing boundary is: only the
first valid copy of a shred identity is a ledger byte.

## Decision

`shred_ingest_packet` is the reconstruction ingest:

```text
net_packet_t
 → shred_envelope
 → shred_key_t / shred_identity_t
 → shred_race_claim
      WIN → shred_prefix_push immediately
      DUP → source_id, winner_source, delta_ns, delta_tsc only
      BAD → drop
```

No wait window. OrbitFlare winning does not delay for DoubleZero.

Identity is Agave `ShredId` (slot, index, Data|Code) plus wire
`fec_set_index` (u32). The variant byte is the type field so merkle
and coding shreds cannot collide. Version is parsed, not keyed.

`rx_sink` still claims before capture-sink; it does not own prefix
state. Prefix slot watching stays with the reconstruction caller.

## Alternatives Considered

- Wait N microseconds then pick the earlier timestamp: rejected; that
  is a sorting stage.
- Forward every copy into prefix and dedup only by index: rejected;
  hides feed-loss telemetry and wastes the assembler on losers.
- Key on payload hash: rejected; headers already name the shred.

## Consequences

- DoubleZero can be added as a second `net_feed_t` without changing
  this claim/prefix contract.
- Later we can stamp which source completed a DATA_COMPLETE batch or
  a framed tx; that is not this slice.
- Eviction of a live identity can still emit a second WIN.
