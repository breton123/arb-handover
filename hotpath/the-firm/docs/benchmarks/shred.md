# Shred parse + keyed claim baseline

Host: WSL2 Ubuntu, gcc 15.2, `-O3 -march=native`. 2026-09-26.

Intra-process `rdtscp`. Not NIC latency. Not a wire race.

## `shred_bench` (200k after 1k warmup)

Alternating OrbitFlare / DoubleZero copies of the same identity.

| Stage | p50 | p99 | max |
| --- | ---: | ---: | ---: |
| `shred_identify` (header only) | 20 ns | 20 ns | 17 us |
| `shred_race_claim` (win or dup) | 30 ns | 430 ns | 31 us |

Observed: OF first 100500, DZ late 100500 (OF always presented first per identity).

Sanitizer tests passed separately; this bench is unsanitized.
