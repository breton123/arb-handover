# RX ingest baseline

Host: WSL2 Ubuntu, gcc 15.2, `-O3 -march=native`. 2026-09-26.

These are **not** NIC or OrbitFlare-wire numbers. Sanitizer builds were not used.

## Ring (`rx_bench --mode ring --count 200000`)

1200-byte synthetic payload, 4096-slot SPSC, intra-process `rdtscp`.

| Stage | min | p50 | p95 | p99 | max | mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| push (memcpy + publish) | 20 ns | 30 ns | 90 ns | 260 ns | 35.2 us | 53.3 ns |
| acquire + release | 10 ns | 20 ns | 20 ns | 70 ns | 36.9 us | 19.7 ns |

`tsc_hz` ≈ 3.79e9.

## UDP loopback (`rx_bench --mode udp --count 20000`)

`127.0.0.1`, 1200-byte datagrams, batch 32, 4096 slots.

| | |
| --- | ---: |
| received / expected | 20000 / 20000 |
| sink_bad | 0 |
| ring_drops | 0 |
| window | 93.6 ms |
| packets/s | 213776 |

`feed_poll` burst (recvmmsg + stamp + publish), rdtscp:

| min | p50 | p95 | p99 | max | mean | samples |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1.68 us | 7.67 us | 23.0 us | 38.1 us | 196 us | 9.46 us | 9797 |

Re-run on the isolated Linux capture box before treating any of this as production RX latency.
