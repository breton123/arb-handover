# WIRE-TO-STATE-001

2026-09-26. `./build/wire_to_state_bench`. Intra-process rdtscp.
Synthetic DATA_COMPLETE shred wrapping a framed Pump sell. Not NIC RTT.
Not FEC. Not sanitizers.

tsc_hz ≈ 3.79e9. samples=4000 (400 missing-`V` fallbacks by construction).

## Admission

| | n |
| --- | ---: |
| FAST_CUSTOM | 3600 |
| FALLBACK (`pre_virtual`) | 400 |
| UNKNOWN | 0 |
| tx bytes before prestate | 789 (19.7%) |

## Stages (all candidates, ns)

| stage | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| tx_complete | 310 | 390 | 851 | 22083 |
| OrderedTx | 100 | 130 | 340 | 550 |
| resolve | 30 | 30 | 50 | 540 |
| deps_classify | 20 | 30 | 40 | 90 |
| prestate_ready | 20 | 30 | 40 | 80 |
| overlay/apply | 160 | 230 | 390 | 13944 |
| exactness_cert | 110 | 110 | 150 | 13383 |
| atomic_publish | 110 | 120 | 160 | 18933 |
| e2e | 861 | 1061 | 1893 | 24667 |

## Exact-state e2e (FAST_CUSTOM only)

p50 **891 ns** · p90 1071 · p99 1923 · max 24667

## Budget (sum of stage ns)

reconstruction (tx_complete+OrderedTx) > apply+publish > resolve/classify/pre/cert.

The bottleneck on this fixture is **first-tx extract + decode**, not Pump apply.
Live OrbitFlare + FEC + delayed AUTH will move the mass into `tx_complete`
and `prestate_ready`.
