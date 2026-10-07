# Latency evaluation

## What to measure

Stamp `rdtscp` at each stage boundary and carry the stamps on the item (`net_packet_t.rx_tsc` is the first). Convert
with the calibrated TSC frequency that `the-firm` capture headers already store. Report p50, p90, p99, p99.9 and max
for every stage and for each end-to-end span, per feed.

| Span | From → to |
|---|---|
| ingest | NIC receive (`rx_tsc`) → shred race decision |
| framing | race win → framed transaction available (TXFRAME) |
| resolve | framed → ALT-resolved account list |
| features | resolved → 173-input vector |
| lookup | touched pools → candidate set |
| score | vector → `p_arb`, `size` (full model and compact model separately) |
| decide | scores → decision and chosen routes |
| build | decision → patched template (per pack) |
| sign | template → signed bytes (Ed25519) |
| **decision path** | framed → decided |
| **end to end** | `rx_tsc` → signed bytes of the first pack |

## Targets

| Span | Target p99 | Note |
|---|---|---|
| resolve (cache hit) | ≤ 200 ns | miss means not scored |
| features | ≤ 1 µs | integer work plus one hash lookup per key |
| lookup | ≤ 300 ns | perfect hash per resolved key |
| score, compact model | ≤ 1 µs | full model reported for comparison |
| **decision path** | **≤ 2 µs p50, ≤ 5 µs p99** | |
| sign | ≈ 15 µs p50 today | libsodium; consider pre-signing work where possible |
| **end to end** | **≤ 1 ms p99** | |

Arrival-time context matters as much as compute: report, per feed, the share of shreds won and the time from slot
start to the triggering transaction being framed. The model can only act on what has been framed.

## Method

- Pin each thread to an isolated core; record the cores and the CPU governor in every report.
- Warm up for 60 seconds before measuring; measure for at least one hour of live traffic.
- Use the replay mode on a fixed capture for before/after comparisons; use live runs for the headline numbers.
- Keep the measurement off the hot path: stamps are written into the item, the logger thread computes deltas.

## Benchmarks already in the tree

`the-firm/docs/benchmarks/` (`rx.md`, `shred.md`, `rs_fast.md`, `state.md`, `wire_to_state*.md`) and the EXEC-00x
measurements in `arb-exec/README.md` (sign ≈ 15 µs p50, nonce claim ≈ 20 ns). Re-run them on the current box
before comparing.
