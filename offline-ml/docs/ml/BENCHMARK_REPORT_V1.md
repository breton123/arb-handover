# framed-tx-v1 encode benchmark

Schema `framed-tx-v1`. Corpus: `raw-tx-snippet.jsonl` (8 transactions).

## Method

- `cargo bench -p tx-features --bench encode --features bench-harness`. Profile inherits release: `opt-level=3`, thin LTO, `codegen-units=1`, `panic=unwind`. The `phase-profile` feature was off for this table.
- Thread affinity pin: succeeded, mask bit 2, priority 2.
- TSC frequency from `rdtsc` over ~100 ms of wall time: 3.793 GHz. Empty `rdtsc` pair median: 38 cycles, subtracted from single-shot samples only.
- Each transaction: 5 000 untimed warmup calls, a tight batch of 50000 calls, then 20000 single-shot samples.
- The batch mean is the cost estimate. It has no per-call timer inside the loop. Single-shot percentiles still include residual measurement noise after the overhead subtraction.
- A counting allocator is armed only around the batch and the samples. Scratch, output, and the sample buffer are allocated before the counter is armed.
- Bytes copied are instruction data heads (at most 32 bytes each). Pubkeys are hashed in place.
- Figures are the measured values. They were not edited toward 1 µs.

## Observed pilot maxima

These are this file only. Capacities are the protocol bounds in `FRAMED_TX_FEATURES_V1.md`, not these maxima.

| quantity | observed max | V1 capacity |
|---|---:|---:|
| raw bytes | 1050 | 1232-byte packet is the current wire budget; the encoder accepts more until a table fills |
| instructions | 8 | 768 |
| expanded accounts | 46 | 256 |
| ALT lookups | 2 | 64 |
| signatures | 2 | not stored |
| max instruction data bytes | 217 | shortvec u16 |
| max accounts in one instruction | 36 | shortvec u16 |

## Per transaction

| tx | bytes | version | ixs | accounts | alt | batch ns | cycles/tx | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | allocs/tx | bytes copied |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 780 | legacy | 4 | 19 | 0 | 539.7 | 2046.8 | 531.0 | 561.1 | 921.7 | 1102.1 | 1852970 | 0.000 | 32 |
| 1 | 1010 | v0 | 8 | 46 | 1 | 1249.4 | 4738.6 | 1182.2 | 1202.3 | 1973.7 | 2524.7 | 800405 | 0.000 | 133 |
| 2 | 797 | v0 | 5 | 27 | 1 | 750.3 | 2845.7 | 721.4 | 731.4 | 1152.2 | 1903.6 | 1332793 | 0.000 | 52 |
| 3 | 250 | legacy | 2 | 2 | 0 | 129.5 | 491.2 | 130.2 | 130.2 | 210.4 | 320.6 | 7719742 | 0.000 | 41 |
| 4 | 642 | legacy | 6 | 9 | 0 | 432.7 | 1641.2 | 410.8 | 611.1 | 731.4 | 1021.9 | 2310856 | 0.000 | 138 |
| 5 | 590 | v0 | 5 | 21 | 1 | 607.4 | 2303.5 | 581.1 | 591.1 | 841.6 | 1753.3 | 1646486 | 0.000 | 56 |
| 6 | 823 | v0 | 8 | 28 | 1 | 865.4 | 3282.1 | 871.6 | 1292.4 | 1442.7 | 3346.3 | 1155591 | 0.000 | 66 |
| 7 | 1050 | legacy | 4 | 27 | 0 | 908.0 | 3443.9 | 671.3 | 691.3 | 1042.0 | 2454.6 | 1101288 | 0.000 | 40 |

## Buckets

p50–p99.9 pool every single-shot sample in the bucket. `batch ns` and `cycles/tx` are the unweighted mean of the per-transaction batch measurements. `tx/s` is `1e9 / batch ns`.

| bucket | txs | batch ns | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | cycles/tx | allocs/tx | bytes copied |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| all | 8 | 685.3 | 661.2 | 1182.2 | 1583.0 | 2114.0 | 1459225 | 2599.1 | 0.000 | 69.8 |
| legacy | 4 | 502.5 | 521.0 | 681.3 | 951.8 | 1212.3 | 1990070 | 1905.8 | 0.000 | 62.8 |
| v0 | 4 | 868.1 | 841.6 | 1212.3 | 1843.5 | 2394.5 | 1151946 | 3292.5 | 0.000 | 76.8 |
| no ALT | 4 | 502.5 | 521.0 | 681.3 | 951.8 | 1212.3 | 1990070 | 1905.8 | 0.000 | 62.8 |
| ALT | 4 | 868.1 | 841.6 | 1212.3 | 1843.5 | 2394.5 | 1151946 | 3292.5 | 0.000 | 76.8 |
| bytes 0-399 | 1 | 129.5 | 130.2 | 130.2 | 210.4 | 320.6 | 7719742 | 491.2 | 0.000 | 41.0 |
| bytes 400-799 | 4 | 582.5 | 581.1 | 721.4 | 1062.0 | 1573.0 | 1716685 | 2209.3 | 0.000 | 69.5 |
| bytes 800+ | 3 | 1007.6 | 1001.9 | 1242.3 | 1883.5 | 2594.9 | 992472 | 3821.5 | 0.000 | 79.7 |
| ix 1-3 | 1 | 129.5 | 130.2 | 130.2 | 210.4 | 320.6 | 7719742 | 491.2 | 0.000 | 41.0 |
| ix 4-8 | 7 | 764.7 | 671.3 | 1182.2 | 1653.1 | 2164.1 | 1307720 | 2900.2 | 0.000 | 73.9 |
| ix 9+ | 0 | | | | | | | | | |

## Reading

Aspirational target for common transactions: single-shot p50 under 1000 ns, and 0 heap allocations on a successful encode.

On this pilot, 7 of 8 transactions have p50 under 1000 ns. The miss is transaction 1: p50 1182.2 ns, batch mean 1249.4 ns, 1010 bytes, v0, 8 instructions, 46 expanded accounts, 2 address-lookup tables. The unweighted mean batch time across all 8 is 685.3 ns. The fastest is transaction 3 at 129.5 ns. Every timed encode recorded 0.000 heap allocations. The bytes-copied column is the retained instruction heads only. `identity_hash64` also copies 8-byte lanes onto the stack while hashing static keys; those copies are not in that column.

## Where the time goes

A second build with `--features bench-harness,phase-profile` stores four `rdtsc` deltas inside `encode_transaction_v1` (wire parse, instruction derivation, shared-account scan, rollup and fingerprints). That build is not the table above: each encode pays for four extra timer pairs and four atomic adds, so its absolute nanoseconds are higher. The split below is the share of those instrumented cycles.

| tx | parse | instructions | shared scan | rollup and fingerprints |
|---:|---:|---:|---:|---:|
| 0 | 26% | 33% | 5% | 36% |
| 1 | 21% | 36% | 5% | 38% |
| 2 | 25% | 33% | 5% | 37% |
| 3 | 29% | 38% | 9% | 23% |
| 4 | 25% | 44% | 7% | 24% |
| 5 | 25% | 34% | 5% | 37% |
| 6 | 24% | 37% | 5% | 34% |
| 7 | 27% | 30% | 4% | 39% |

On the transaction that misses 1 µs, no single phase is the majority. Instruction derivation is 36% (account-list walks, prefix/head copy, structural-family hash, per-account usage stamps). Rollup and fingerprints are 38% (one pass over the expanded account table, then the program-sequence, structure, and topology mixes). Wire parse is 21%, and that includes the identity hash of every static pubkey. The second shared-account scan is 5%. Cost tracks expanded account count and instruction count.
