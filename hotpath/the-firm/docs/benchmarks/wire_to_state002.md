# WIRE-TO-STATE-002

Host: Frankfurt `195.242.152.178`, gcc `-O3 -march=native`. 2026-09-26.
Sanitizer builds were not used. No Reed-Solomon.

## Self-test

```text
./build/wire_to_state002 --self-test
```

Two-shred synthetic Pump sell: multi-shred bucket, `tx_before_pre=1`,
FAST_CUSTOM=0 (empty CompactState). `packet_rx → tx_complete` = 114 ns
(synthetic TSC gap 400 cycles @ 3.5 GHz).

## Real FEEDCAP1

```text
./build/wire_to_state002 --cap /data/bsc/captures/orbitflare/orbitflare-20260926-224724.cap
```

| | |
| --- | ---: |
| kind | FEEDCAP1 |
| tsc_hz | 4.194e9 |
| packets | 1 732 555 |
| one-packet complete | 316 |
| multi-shred complete (no FEC) | 403 |
| FEC-recovered | 0 |
| fec-possible (coding-enough) | 376 262 |
| incomplete data shreds | 7 157 |
| framed txs extracted | 1 |
| empty DATA_COMPLETE (ticks) | 718 |
| Pump / ALT / FAST_CUSTOM | 0 / 0 / 0 |

Capture-TSC headlines:

| Stage | n | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| packet_rx → data_complete (no FEC) | 719 | 1.0 µs | 7.7 ms | 23.2 ms | 53.0 ms |
| packet_rx → tx_complete (framed) | 1 | 404 µs | 404 µs | 404 µs | 404 µs |
| packet_rx → exact publish | 0 | — | — | — | — |
| tx_complete → exact publish | 0 | — | — | — | — |

## What this means

Concat of contiguous DATA_COMPLETE shreds is not the live Pump path.
~376k FEC sets had enough shards to recover and we did not recover them.
Until Reed-Solomon exists, FAST_CUSTOM Pump first-pkt→publish cannot be
measured on this traffic. Caps also carry no certified AUTH, so exact
publish would be 0 even after recover.

The 719 no-FEC completes are mostly ticks. p50 ~1 µs is one-packet
DATA_COMPLETE. The tail is waiting for later data shreds of the same run.

DLMM stays blocked.
