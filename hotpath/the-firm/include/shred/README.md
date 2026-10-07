# shred — envelope + keyed race

## Purpose

Name a Solana shred from its header and decide, immediately, whether this
copy is the first arrival of that shred or a later duplicate.

## Responsibilities

- Envelope parse (variant, slot, index, version, FEC set)
- Header-derived `shred_identity_t`
- First-writer-wins table
- Duplicate timing telemetry (`% first`, pair deltas)

## Non-responsibilities

- FEC recovery or Reed-Solomon
- Reed-Solomon recover + Merkle-root check (`shred/fec.h`)
  when enough shards exist; fail closed on conflicting roots
- Full slot reconstruction beyond contiguous DATA_COMPLETE concat
  (`shred/assemble.h`) and first-tx extract (`shred/complete.h`)
- Signature verification
- Protocol or pool state
- Owning UDP sockets or NIC RX
- Adding DoubleZero / Blockspace feeds
- Waiting for other feeds before releasing a winner

## Inputs

`net_packet_t` (or raw bytes + `source_id` + `rx_ns`).

## Outputs

| Verdict | Downstream | Telemetry |
| --- | --- | --- |
| `SHRED_RACE_WIN` | yes, immediately | `first[source]++` |
| `SHRED_RACE_DUP` | no | `late[source]++`, pair delta |
| `SHRED_RACE_BAD` | no | `bad++` |

Identity (not a payload hash):

```text
slot, index, fec_set (u32 wire), shred_type
```

## Public API

| Header | Contract |
| --- | --- |
| `shred/shred.h` | `shred_parse`, `shred_identify` |
| `shred/race.h` | `shred_race_claim`, stats, pair percentiles |

## Data ownership

| Object | Created by | Writer | Reader |
| --- | --- | --- | --- |
| race table | app, startup | one race/sink thread | telemetry snapshot |
| entries | table | same writer | same |
| sample ring | table | writer on DUP | cold-path `pair_pct` |

No `malloc` on `shred_race_claim`. `pair_pct` may allocate on the 1 Hz path.

## Thread ownership

The sink/race thread: mux-acquire → parse/claim → maybe sink → release.
RX threads never wait on this table.

## Memory ownership

Default table is 2^17 slots (~a few MB) plus a 64 Ki sample ring, allocated
once. Stale entries (slot lag ≥ 64) are reused.

## Failure behaviour

- Unparseable datagram: `BAD`, not forwarded.
- Table pressure: evict oldest probe slot, count `evict`, still `WIN` the new identity.
- Same source claiming twice: `DUP`, late++, no extra pair sample.

## Performance expectations

Header parse is a few loads. Claim is open-addressed probe ≤ 16.
Measure with `build/shred_bench`. Not NIC latency.
See `docs/benchmarks/shred.md`.

## Testing

`test_shred_parse`, `test_shred_race`, `test_rx` (loopback + claim).

## How this enables feed #2

OrbitFlare already claims into this table. A DoubleZero `net_feed_t` adds
a second ring to the mux. The first valid identity wins; the other copy
only updates `first%` and OF↔DZ deltas. Do not add that feed until you
want those numbers.
