# apply_tx and pipeline latency

Host: WSL2 Linux, `gcc -O3 -march=native`, intra-process `rdtscp`.
Not shred→reconstruct→publish. Not under sanitizers.

## `state_bench` — `apply_tx` on Pump vector legs

20 000 timed samples (2 000 warm). Includes classify, lookup, overlay,
publish, certificate.

| metric | ns |
| --- | ---: |
| min | 250 |
| p50 | 370 |
| p95 | 911 |
| p99 | 1051 |
| max | 285035 |
| mean | 459.2 |

p99 is still ~1 µs. The max is a tail spike on this host, not the
steady-state budget.

## `state_pipeline_bench` — staged wrap + Pump sell

8 000 samples. Seed/reset is outside the timed window.

| stage | p50 ns | p99 ns |
| --- | ---: | ---: |
| classify | 20 | 50 |
| dep_lookup | 40 | 90 |
| overlay_clone | 140 | 250 |
| pump_exec | 60 | 110 |
| token_update | 70 | 120 |
| publish | 200 | 260 |
| **total** | **530** | **801** |

Clone + publish dominate once token rows are in the overlay. Pump
execution stays cheap. Still not a live reconstruction budget.
