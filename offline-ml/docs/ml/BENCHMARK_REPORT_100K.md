# framed-tx-v1 encode benchmark, 100k corpus

Schema `framed-tx-v1`. Input `c:\Users\louis\Desktop\the-firm-offline\raw-tx-100k.jsonl`.

## Method

- `cargo bench -p tx-features --bench encode --features bench-harness -- --input raw-tx-100k.jsonl`. Profile inherits release: `opt-level=3`, thin LTO, `codegen-units=1`, `panic=unwind`. `phase-profile` was off.
- Thread affinity pin: succeeded, mask bit 2, priority 2.
- TSC frequency from `rdtsc` over ~100 ms of wall time: 3.793 GHz. Empty `rdtsc` pair median: 38 cycles, subtracted from single-shot samples only.
- Hex decode and JSONL parsing happen before the timed section. The timed calls see only raw transaction bytes, the same scratch, and the same output buffer.
- Two untimed warmup passes over every successful transaction.
- Each successful transaction is then encoded 64 times in a tight loop. One timer covers those 64 calls. The tables below use that per-transaction mean.
- A separate single-shot pass records one `rdtsc` pair per transaction, minus the empty-pair median. Those percentiles still include residual timer noise.
- Throughput is 8 back-to-back passes over the successful corpus inside one timer. `ns/tx` and `cycles/tx` in the throughput line divide that one interval by the number of encodes.
- A counting allocator is armed around one extra full pass. Scratch, output, and the decoded transactions are allocated before that pass.
- Bytes copied are instruction data heads (at most 32 bytes each). Pubkey identity hashes copy 8-byte lanes on the stack and are not included.
- Transactions that do not return `OK` are counted and left out of the latency tables.
- Figures are the measured values. They were not edited toward 1 µs.

## Encode status

| status | count |
|---|---:|
| OK | 100000 |
| MALFORMED | 0 |
| UNSUPPORTED_VERSION | 0 |
| CAPACITY_EXCEEDED | 0 |
| NULL_ARGUMENT | 0 |
| INTERNAL | 0 |

## Observed maxima

Successful encodes only. Capacities are the protocol bounds in `FRAMED_TX_FEATURES_V1.md`.

| quantity | observed max | V1 capacity |
|---|---:|---:|
| raw bytes | 1232 | 1232-byte packet is the current wire budget; the encoder accepts more until a table fills |
| instructions | 36 | 768 |
| expanded accounts | 64 | 256 |
| ALT lookups | 10 | 64 |
| signatures | 8 | not stored |
| max instruction data bytes | 973 | shortvec u16 |
| max accounts in one instruction | 119 | shortvec u16 |

## Throughput

One timer around 8 passes of 100000 successful transactions (2057008 tx/s, 486.1 ns/tx, 1843.9 cycles/tx). Heap allocations on a separate full pass: 0.000 per encode.

## Buckets

Each row's latency is its 64-call batch mean. p50–p99.9 are percentiles of those means across the transactions in the bucket. `batch ns` is their unweighted mean. `tx/s` is `1e9 / batch ns`. The throughput line above is the tighter measurement.

| bucket | txs | batch ns | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | cycles/tx | bytes copied |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| all | 100000 | 376.7 | 178.1 | 1062.5 | 1793.8 | 2189.1 | 2654605 | 1426.0 | 40.5 |
| legacy | 70655 | 191.2 | 107.8 | 356.2 | 1114.1 | 1557.8 | 5229760 | 722.6 | 35.2 |
| v0 | 29345 | 823.3 | 776.6 | 1370.3 | 2046.9 | 2292.2 | 1214600 | 3119.5 | 53.5 |
| no ALT | 85978 | 247.6 | 109.4 | 632.8 | 1184.4 | 1621.9 | 4039085 | 936.3 | 37.6 |
| ALT | 14022 | 1168.4 | 1175.0 | 1642.2 | 2135.9 | 2340.6 | 855841 | 4428.2 | 58.6 |
| bytes 0-399 | 64640 | 137.6 | 106.2 | 209.4 | 360.9 | 679.7 | 7265326 | 519.4 | 33.1 |
| bytes 400-799 | 17554 | 682.0 | 598.4 | 1270.3 | 1882.8 | 2073.4 | 1466266 | 2583.7 | 50.0 |
| bytes 800-1232 | 17806 | 943.6 | 881.2 | 1396.9 | 2110.9 | 2334.4 | 1059791 | 3575.5 | 58.3 |
| bytes 1233+ | 0 | | | | | | | | |
| ix 1-3 | 67844 | 151.9 | 107.8 | 218.8 | 692.2 | 1239.1 | 6583973 | 573.4 | 32.9 |
| ix 4-8 | 30328 | 835.3 | 773.4 | 1356.2 | 2031.2 | 2262.5 | 1197244 | 3164.8 | 53.2 |
| ix 9-16 | 1471 | 1038.2 | 998.4 | 1504.7 | 2009.4 | 2465.6 | 963242 | 3934.1 | 96.1 |
| ix 17+ | 357 | 1421.1 | 1304.7 | 1965.6 | 2212.5 | 3264.1 | 703689 | 5386.4 | 181.8 |

## Single-shot percentiles

One timed call per transaction. The empty `rdtsc` pair median is subtracted. Residual timer noise remains, so these sit above the batch means.

| bucket | txs | p50 ns | p90 ns | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|---:|
| all | 100000 | 250.5 | 1292.4 | 2073.9 | 2835.3 |
| legacy | 70655 | 190.4 | 531.0 | 1302.4 | 2023.8 |
| v0 | 29345 | 931.7 | 1683.2 | 2384.5 | 3757.1 |
| no ALT | 85978 | 210.4 | 771.4 | 1352.5 | 2073.9 |
| ALT | 14022 | 1402.6 | 1923.6 | 2574.8 | 4809.0 |
| bytes 0-399 | 64640 | 190.4 | 330.6 | 581.1 | 851.6 |
| bytes 400-799 | 17554 | 711.3 | 1462.7 | 2104.0 | 3115.9 |
| bytes 800+ | 17806 | 1072.0 | 1773.3 | 2494.7 | 3767.1 |
| ix 1-3 | 67844 | 190.4 | 370.7 | 901.7 | 1823.4 |
| ix 4-8 | 30328 | 921.7 | 1643.1 | 2324.4 | 3757.1 |
| ix 9+ | 1828 | 1312.5 | 2003.8 | 2895.4 | 3767.1 |

## Reading

Aspirational target for common transactions: under 1000 ns. The full-corpus throughput measurement is 486.1 ns/tx. The unweighted mean of the per-transaction 64-call batches is 376.7 ns, with p50 178.1 ns and p99 1793.8 ns. Heap allocations per encode on the counted pass: 0.000.
