# Ingest packet boundary and feed-race ownership

## Context

The old `arb-feed` path delivered a frozen `rx_packet_t` of `{data, len, queue}`.
Timestamps lived only on the capture record. That kept downstream source-agnostic
but cannot express first-arrival racing across OrbitFlare, DoubleZero, and
Blockspace.

Production ingest on the old box was single-`recv` UDP plus a lossy SPSC capture
ring. The proven low-latency receive primitive lived in `arb-nic` (`recvmmsg`,
optional `SO_BUSY_POLL`, CPU pin, `SO_RCVBUF`).

## Decision

1. The downstream ingest item is `net_packet_t`: `source_id`, `rx_ns`, `rx_tsc`,
   `len`, and a pointer into a preallocated slot. Timestamps are first-class.
2. Feeds are independent `net_feed_t` instances. Identity is a reserved integer,
   not a callback or vtable. OrbitFlare is `1`; DoubleZero `2`; Blockspace `3`.
3. Each feed writes its own SPSC ingress ring. `recvmmsg` lands directly in
   claimed slot payloads (no stack bounce).
4. `net_race_t` merges ready rings by earliest `rx_ns` and still delivers later
   copies. Keyed dedup is deferred until a shred identity exists.
5. Capture is a new versioned format `FIRMCAP1` that stores `source_id`. It is
   not `FEEDCAP1`. The recorder thread remains off-path and lossy.
6. Linux is the production target. Ring, capture, and race stay portable for
   tests.

## Alternatives Considered

- Keep old `rx_packet_t` and put timestamps only on capture: rejected; racing
  and `% first` need the stamp on the live item.
- Shared multi-writer ingress ring: rejected; would require synchronization
  on the hot path. Per-feed SPSC + merge keeps single-writer ownership.
- Hash-payload dedup now: rejected; hashing on RX is expensive and the right
  key is a shred identity owned by a later subsystem.
- Copy FEEDCAP1 exactly: rejected; adding `source_id` is a format change and
  must not silently break old readers.

## Consequences

- Downstream reconstruction must treat `net_packet_t` as the live/replay
  boundary, not `{data,len,queue}`.
- Old FEEDCAP1 files are not readable by this recorder; analysis of historical
  OrbitFlare captures stays on the old tools.
- Feed #2 is an additive open + ring + `net_race_add`. OrbitFlare RX does not
  change.
- `% first` counters exist but are only meaningful after keyed race lands.
