# net — data-plane ingest

## Purpose

Receive raw packets from independent market-data feeds, stamp first-arrival
time and source identity, and hand a compact descriptor to downstream.

OrbitFlare is feed #1. The same types are how DoubleZero, Blockspace, and
later feeds attach.

## Responsibilities

- IPv4 UDP bind and batched receive (`recvmmsg`)
- Preallocated SPSC ingress rings
- Per-packet `source_id`, monotonic timestamp, length, payload reference
- Lossy off-path raw capture (`FIRMCAP1`)
- Lightweight atomic counters
- Round-robin mux of ready ingress rings (no timestamp sort)

## Non-responsibilities

- Shred parsing, FEC, or reconstruction (`src/shred`)
- Keyed first-writer-wins (`shred_race_claim`)
- Protocol / pool state
- Strategy, signing, or sending
- AF_XDP / NIC zero-copy
- Retention / disk-watch scripts

## Inputs

- UDP datagrams on a configured bind address and port
- Test/replay inject via `net_ring_push`

## Outputs

`net_packet_t` at the downstream boundary:

| Field | Meaning |
| --- | --- |
| `source_id` | Stable feed identifier |
| `rx_ns` | `CLOCK_MONOTONIC_RAW` at the receive burst |
| `rx_tsc` | `rdtscp` at the same instant |
| `len` | Payload bytes |
| `data` | Alias of the ring-slot payload |

`data` is valid only until `net_ring_release` / `net_race_release`.

## Public API

| Header | Contract |
| --- | --- |
| `net/packet.h` | Source IDs, `net_packet_t`, `net_slot_t` |
| `net/ring.h` | Claim/commit, acquire/release, lossy push |
| `net/feed.h` | One RX feed (`net_orbitflare_open`, `net_feed_poll`) |
| `net/race.h` | Mux: next ready packet, round-robin, no wait |
| `net/capture.h` | `FIRMCAP1` + recorder thread |
| `net/stats.h` | Atomic counters + snapshot |
| `net/sink.h` | Temporary validation consumer |

Source IDs are assigned at compile time:

```text
1  OrbitFlare   (implemented)
2  DoubleZero   (reserved)
3  Blockspace   (reserved)
4–7 reserved
```

Adding feed #2 is a new `net_feed_t` with `NET_SOURCE_DOUBLEZERO`. Do not
edit OrbitFlare-specific code.

## Data ownership

| Object | Created by | Writer | Reader | Lifetime |
| --- | --- | --- | --- | --- |
| `net_ring_t` slots | app, startup `calloc` | one RX thread | one consumer | until `net_ring_free` |
| claimed slot | RX after `claim` | RX until `commit` | none | unpublished if `abort` |
| acquired packet | consumer `acquire` | none | consumer until `release` | pointer aliases slot |
| capture ring | app, optional | RX `try_push` copy | recorder thread | lossy; RX never waits |
| `net_feed_t` | app, static/heap once | RX thread | — | large (discard batch) |

No `malloc`/`free` on the steady-state RX path.

## Thread ownership

```text
RX core        net_feed_poll  (optional --cpu)
sink core      mux-acquire → shred_race_claim → winners only  (optional --sink-cpu)
recorder core  FIRMCAP1 write, lossy  (optional --rec-cpu)
telemetry      1 Hz snapshot on the RX thread after a burst, not per packet
```

No mutex on the packet path. Rings are SPSC atomics.

## Memory ownership

Rings and the feed object are allocated at startup. Slot payloads are
the `recvmmsg` destination (one kernel copy into the ingress slot).
Capture takes a second memcpy into a lossy side ring so disk I/O cannot
stall RX or the sink.

## Failure behaviour

- Malformed / empty datagrams increment `malformed` and are not published.
- Full ingress ring: socket is still drained into a discard batch; `ring_drops` increases. Writer never blocks.
- Full capture ring: increment `cap_drops`, continue. Recorder never blocks RX.
- Truncation (`MSG_TRUNC` or `len > NET_PKT_MAX`): flag `NET_PKT_TRUNC`, count `trunc`.
- Unexpected socket errors return `-1` from `net_feed_poll`.

## Performance expectations

- Default batch 32, `SO_RCVBUF` 16 MiB, ring 32768 slots (~68 MiB).
- Poll + `recvmmsg` is the signal-friendly path; `--busy` adds userspace spin and `SO_BUSY_POLL`.
- Measure with `build/rx_bench`. Do not treat loopback or sanitizer builds as NIC latency.

## Testing

```text
make test
# or
cmake --build build && ctest --test-dir build
```

| Test | Coverage |
| --- | --- |
| `test_ring` | source IDs, push/pop, drop, claim/commit |
| `test_capture` | FIRMCAP1 round-trip, recorder drain |
| `test_race` | mux claims on arrival; does not sort by `rx_ns` |
| `test_rx` | Linux OrbitFlare UDP → ring → shred claim → sink |

## How feed #2 plugs in

```text
OrbitFlare  ─┐
DoubleZero  ─┼─> each: socket + net_feed_t + own ingress ring
Blockspace  ─┘
                 ↓
            mux (first ready ring)
                 ↓
            shred envelope + keyed claim
              /                    \
           WIN                      DUP
            ↓                        ↓
     reconstruction            telemetry only
```

Do not add DoubleZero until you want `% first` / pair deltas. The table
is already the claim point. See `src/shred/README.md`.

## Apps

```text
build/rx_sink --bind 0.0.0.0 --port 20001 [--out DIR] [--cpu 47] [--rec-cpu 46]
build/rx_bench --mode ring
build/rx_bench --mode udp --count 20000   # Linux
```
