# Paper-trading specification

## What it is

A shadow process on the live feed that makes every decision the production path would make, logs it with
timestamps, and never transmits anything. Later, the log is joined to ground truth to measure what we would have
earned.

## Process

```text
live feeds ─▶ the-firm ingest + race + TXFRAME + ALT ─▶ feature builder ─▶ pool lookup ─▶ score ─▶ decide
                                                                                               │
                                                     decision log (append-only, off the hot path) ◀┘
                                                                                               │
                                       M5: arb-exec template + nonce + sign (no send) ◀────────┘
```

- Run on the Frankfurt box, on isolated cores. Do not bind OrbitFlare port `:20001` if the capture stack owns it;
  use the capture fan-out or a second subscription (see `the-firm/docs/adr/0019-live-state.md`).
- Load the model bundle at start-up by path. Log the bundle `manifest.json` sha256 once per run.
- The logger runs on its own thread through a lossy SPSC ring, the same pattern as `FIRMCAP1` capture. A full ring
  drops records and counts them; it never blocks the decision path.

## Decision log schema

One record per decision, plus a deterministic 1-in-1000 sample of transactions that were scored and not sent
(`sampled = 1`), and counters for everything else.

| Field | Type | Meaning |
|---|---|---|
| `run_id` | string | per process start |
| `bundle_sha256` | string | `manifest.json` hash |
| `slot`, `entry_index`, `tx_index` | u64 | position of the triggering transaction |
| `signature` | base58 | first signature of the triggering transaction |
| `source_id` | u8 | feed that delivered the winning shred (1 OrbitFlare, 2 DoubleZero, 3 Blockspace) |
| `tsc_rx`, `tsc_framed`, `tsc_resolved`, `tsc_features`, `tsc_scored`, `tsc_decided`, `tsc_signed` | u64 | `rdtscp` at each stage (`tsc_signed` only in M5) |
| `rx_ns` | u64 | `CLOCK_MONOTONIC_RAW` at receive |
| `p_arb`, `size_log`, `ev` | f64 | model outputs |
| `budget`, `threshold`, `L` | | policy in force |
| `sent` | bool | decision (false for sampled non-decisions) |
| `n_pools`, `n_candidates` | u32 | touched pools and candidate routes |
| `routes` | list of route ids | the `3 × L` routes chosen |
| `sampled` | bool | 1 for the non-decision sample |
| `features_digest` | u64 | hash of the 173-input vector (for offline replay checks) |

Write Parquet (or a binary format with a converter) in hourly files. Keep every file.

Counters every second: packets by feed, shred wins by feed, framed transactions, framing failures by cause, ALT
misses, transactions with candidates, decisions, log drops.

## Ground truth and scoring (M4)

Three sources, in order of availability:

1. **Realised arbs on chain** (minutes). For each decision, look at transactions in the next N slots that touch
   the decision's pools. A realised arb is a successful transaction whose fee payer ends with more WSOL after a
   cycle through those pools. Profit comes from pre/post token balances (`getTransaction` off-path, or the collector's
   runtime data). This measures "there was money here and someone took it": the target set we assume we would win.
2. **Collector runtime data** (hours to days). The collector's `runtime.parquet` holds pre/post balances and inner
   instructions for every transaction; it gives the same answer as (1) without RPC.
3. **Labeller labels** (after the labeller runs on that window). Exact counterfactual labels: POSITIVE /
   CERTIFIED_NEGATIVE, profitable routes and their profits. Coordinate the window with the owner so a paper run
   gets labelled.

Metrics, per hour and over the run, with 95% confidence intervals:

- decisions per hour; share with a realised arb on the same pools within N slots; share with a labelled POSITIVE
- value targeted per hour (sum of profit of positives where a chosen route was profitable)
- value available per hour (all positives in the window) and the share targeted
- send mix by label group (POSITIVE, CERTIFIED_NEGATIVE, NO_LABEL, UNRESOLVED) once labels exist
- largest misses: the 50 most valuable positives not decided, each with a cause (no candidate, below threshold,
  ALT miss, framed too late, feature mismatch)

The offline expectation for each metric is in the bundle's `eval.json` (`operating_points["1000"]` for the
1,000/h budget). Report live against it side by side.

## Replay mode

The same binary must run on a recorded capture (`FIRMCAP1` or the TXFRAME fixture) and produce the same decision
log as live, deterministically. Use replay for every code change before it goes live, and to compare against the
offline Python scoring on identical transactions (`features_digest` must match).
