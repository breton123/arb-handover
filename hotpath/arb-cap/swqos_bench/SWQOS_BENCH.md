# SWQOS-BENCH — memo landing, Frankfurt

Standalone mainnet benchmark of the existing production SWQOS path.
Harmless Memo + compute-budget only. No DEX. No OUR_EXEC.
Frozen `leader_send()`, route0, CORE, and arb logic were not modified.

## Methodology

Clock: `CLOCK_MONOTONIC_RAW` for T0–T3. RDTSCP sampled at T0 (tsc_hz 4.194e9).
Signer: production libsodium `exec_sign` (same as EXEC-005A).
Send: existing `swqos_send()` on `ultrasend/1` to `send.swqos.com:11000`.

Per attempt, outside the timed section: `getLatestBlockhash` (confirmed),
`smoke_build_memo` (50k CU limit, cu_price 10000). Then:

| | |
|--|--|
| T0 | immediately before `exec_sign` |
| T1 | immediately after `exec_sign` |
| T2 | immediately after `swqos_send` returns (stream write+finish, no receipt wait) |
| T3 | first `getSignatureStatuses` response that is not `value: [null]` |

Poll: 50 ms, 30 s cap. Confirmed/finalized recorded if they arrived in that window.
Pace: 350 ms after each attempt. Not a load test.

**A WARM** — `swqos_open_env()` once, both QUIC connections held, 200 attempts.
**B COLD** — 8 attempts, `swqos_close` + `swqos_open_env` before each send.
Cold samples are never mixed into warm percentiles.

`send_ns` = T2−T1 is `T_signed_ready → swqos_send_return`.
`sign_to_seen_ns` = T3−T0 is sign-start → first RPC observation.
T3 is **not** leader ingress. Polling (50 ms) and RPC visibility add latency.

Wallet `HHNzjTAB…`. Memo program + compute budget only.

## Sample counts

| | attempts | `swqos_send` == 0 | first RPC seen | confirmed |
|--|--|--|--|--|
| WARM | 200 | 4 | 2 | 2 |
| COLD | 8 | 8 | 8 | 8 |

Warm errors: `send_fail` 195, `not_seen` 2, `blockhash` 1.
After attempt 0 (conn 0, landed) and 2 (conn 1, landed), attempts 3–4
finished the stream (`send` 54–60 µs) but never appeared on RPC.
From attempt 5 the held connections returned `send_fail` in ~40 µs
(`open_bi`/write failed). That is the existing persistent path under
this paced memo run. It was not changed.

Cold reconnect-before-each: 8/8 sent, 8/8 confirmed.

## WARM percentiles (successful stream finishes only, n=4)

Units: **µs**. Do not mix with the 195 `send_fail` rows.

```
                     p50     p90     p99     min     max
sign (n=199)        17.5    20.7    28.1    13.7    40.6
signed→SWQOS ret    55.8    60.3    60.3    40.7    60.3
sign→SWQOS ret      73.2    77.5    77.5    57.8    77.5
SWQOS→RPC seen*       3.33s    7.66s    7.66s    3.33s    7.66s
sign→RPC seen*        3.33s    7.66s    7.66s    3.33s    7.66s
```

\* n=2 landed only. RPC first-seen, not leader arrival.

## COLD CONTROL percentiles (n=8, all landed)

```
                     p50     p90     p99     min     max
sign                21.1    21.3    21.4    17.5    21.4   µs
signed→SWQOS ret    46.6    49.1    52.8    31.6    52.8   µs
sign→SWQOS ret      65.6    67.7    71.4    53.0    71.4   µs
SWQOS→RPC seen       645     950    2598     250    2598   ms
sign→RPC seen        645     950    2598     250    2598   ms
```

## The two numbers that matter

**1. T_signed_ready → swqos_send_return** (`send_ns`)

Local/network handoff after signed bytes exist.

| | n | p50 | p90 | min | max |
|--|--|--|--|--|--|
| WARM success | 4 | **55.8 µs** | 60.3 µs | 40.7 µs | 60.3 µs |
| COLD | 8 | **46.6 µs** | 49.1 µs | 31.6 µs | 52.8 µs |

**2. T_sign_start → first RPC observation** (`sign_to_seen_ns`)

Coarse end-to-end **observation** latency (poll + RPC visibility).

| | n | p50 | min | max |
|--|--|--|--|--|
| WARM | 2 | 7.66 s | 3.33 s | 7.66 s |
| COLD | 8 | **645 ms** | 250 ms | 2.60 s |

## Landing

| | landing % | notes |
|--|--|--|
| WARM successful sends | 2/4 = 50% | 2 not_seen after stream finish |
| WARM all attempts | 2/200 = 1% | 195 send_fail on the held pool |
| COLD | 8/8 = **100%** | reconnect each time |

Landed slots WARM: `450131321`, `450131383`.
Landed slots COLD: `450131952` … `450132236` (8 slots, ~40 apart).

Conn index (last successful write): warm mostly stuck on conn 1 after
the first pair; cold alternated 0/1 as expected after each reconnect.

## Cost

| | |
|--|--|
| Wallet Δ | −55 000 lamports (−0.000055 SOL) |
| Recovered `meta.fee` | 55 000 lamports (11 × 5500) |
| SWQOS prepaid Δ | −1 800 000 lamports (−0.001800 SOL) |
| Implied SWQOS unit | 150 000 lamports / accepted (12 accepts) |

Base fee 5000 + ~500 priority on the memo. No meaningful SOL transfer.

## vs shred-V1 timing budgets

Compared to `send_ns` only (signed bytes → stream finished).

| budget | WARM success (4) | COLD (8) |
|--|--|--|
| 100 µs | 4/4 | 8/8 |
| 250 µs | 4/4 | 8/8 |
| 500 µs | 4/4 | 8/8 |
| 1 ms | 4/4 | 8/8 |
| 2 / 5 / 10 ms | 4/4 | 8/8 |

The held-connection **handoff** is tens of microseconds when `swqos_send`
returns 0. It does not survive 200 paced memos on one open: 195 later
calls fail closed. Reconnect-per-send (cold) keeps both the ~47 µs
handoff and 100% observation landing in this sample.

RPC first-seen (250 ms–8 s) is a different quantity. It is far above
the 100 µs–10 ms shred budgets because it includes poll interval and
cluster visibility, not TPU ingress.

## Raw output

```
arb-cap/swqos_bench/warm.jsonl
arb-cap/swqos_bench/warm.csv
arb-cap/swqos_bench/cold.jsonl
arb-cap/swqos_bench/cold.csv
arb-cap/swqos_bench/run.log          (Frankfurt)
```

Binary: `arb-exec/tests/exec_swqos_bench.c` → `./build/exec_swqos_bench`.
Nothing in the send path was tuned after this run.
