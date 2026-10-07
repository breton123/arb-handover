# Shred identity and first-writer-wins race

## Context

Ingest already delivers `net_packet_t` with `source_id` and `rx_ns` from
independent rings. The first mux compared ready rings by earliest timestamp.
That must not become a stage that buffers and sorts. `rx_ns` is evidence
for telemetry, not a release condition.

Racing DoubleZero or Blockspace is only useful once two copies of the same
shred can be recognized without hashing 1.2 KB.

The old tree already had a frozen envelope parse (`slot`, `index`, `fec_set`,
`type`) and treated FEC reconstruction as a later concern.

## Decision

1. Port the proven envelope parse into `src/shred/`. No FEC, no auth.
2. Race key is `shred_identity_t { slot, index, fec_set, shred_type }`.
   `fec_set` stays `u32` to match the wire. Version is parsed but not keyed.
3. `shred_race_claim` is first-writer-wins on a fixed open-addressed table.
   WIN is forwarded immediately. DUP updates counters and a lossy delta
   sample ring. Never wait for another feed.
4. `net_race_t` merges ready rings round-robin. It does not sort by
   `rx_ns` and does not decide winners. Reconstruction uses
   `shred_ingest_packet` so only WIN copies reach the prefix.
5. One race/sink thread owns the table. Concurrent multi-RX claim is deferred.

## Alternatives Considered

- Keep timestamp-ordered merge: rejected; delays a ready packet to compare
  clocks and invites a sorting stage.
- Hash the full payload: rejected; expensive and unnecessary if headers match.
- `fec_set` as `u16`: rejected; the wire field is 32 bits.
- Claim on each RX thread with CAS: more correct for later multi-feed
  latency, but this slice has one consumer. Adding CAS now is speculative.

## Consequences

- Downstream reconstruction sees each shred at most once (the first valid copy).
- `% first` and pair p50/p95 become meaningful as soon as a second feed exists.
- Eviction of a still-live identity can briefly double-count a WIN.
- Old FEEDCAP1 / classify-on-observe paths remain out of this tree.
