# Hand-over: paper trading and hot-path evaluation

## Goal

Run the current arb model live, in shadow mode, on our own hot path, and answer two questions with
measurements:

1. **Does the model behave live the way it did offline?** Same features, same scores, a similar decision rate and
   a similar share of real arb value targeted.
2. **Is the hot path fast enough to act on it?** Packet arrival to a signed, send-ready transaction, measured on
   our hardware with our feeds.

Assume we win any race we enter (our path is ~2 µs typical, sub-1 ms p99, against competitors in the millisecond
range). What remains to prove is that we target the right transactions, fast, with correct bytes.

## What you get

- **A trained model bundle** (`pool-ev-v1`): arb classifier, size model, pool lookup tables and fitted thresholds
  per send budget. See [docs/02-model-card.md](docs/02-model-card.md) and
  [docs/07-data-and-infra.md](docs/07-data-and-infra.md).
- **The exact feature definitions** and a Rust/Python reference implementation of every input
  ([docs/03-feature-contract.md](docs/03-feature-contract.md)).
- **The decision rule** the offline numbers were measured with ([docs/04-decision-policy.md](docs/04-decision-policy.md)).
- **The hot-path code**: feed race, shred assembly, TXFRAME, ALT cache, templates, signing, nonces, send
  ([docs/01-system-overview.md](docs/01-system-overview.md)).
- **The offline result to beat or match**: on the held-out test window (22.4 chain hours), at about 1,000 decisions
  per hour the bundle targets 32.1% of all labelled arb value: 0.071 SOL per hour (net 0.057) on 3 venues, with
  614 of 11,777 test positives captured
  ([docs/08-results-so-far.md](docs/08-results-so-far.md)). The exact numbers for the exported bundle are in its
  `eval.json`.

## Scope

In scope:

- A C feature builder on the hot path that reproduces the offline features bit for bit.
- Model inference on the hot path, plus a compact model that fits the latency budget.
- A shadow paper trader: live feed → framed tx → features → score → candidate packs → decision log. No sends.
- A send-path dry run: build and sign the transactions for logged decisions, never transmit them.
- Evaluation of paper decisions against ground truth as it becomes available.

Out of scope (owned elsewhere, consume only):

- The labeller, the collector, label schemas and candidate-pack construction. Ask for changes; do not fork them.
- Retraining policy and new feature research. You may train a compact distilled model (M2); anything else goes
  through the owner.
- Any transaction reaching the network. Live sends need explicit approval (M6).

## Milestones and targets

Each milestone ends with a short written report in `reports/` (numbers, method, what failed) and the code that
produced it.

### M0 — Reproduce

- Build `the-firm`, `arb-exec`, `offline-ml` (Rust workspace and Python package) on the Frankfurt box.
- Fetch the bundle and the prepared test window; run `offline-ml/ml/experiments/export_bundle.py`'s evaluation
  again, or score the bundle's models on the test window with `firm_ml`.

**Accept:** test metrics reproduce `eval.json` exactly (same captured count and SOL per hour at each budget).

### M1 — Feature builder in C

- Implement every feature in [docs/03-feature-contract.md](docs/03-feature-contract.md) from a framed raw
  transaction plus the ALT cache plus the bundle's lookup tables. No account or pool state.
- Build a parity harness: run the C builder and the reference (`tx-features` + `ml-prep` + `extract_extras.py`
  logic) over at least 1,000,000 recorded transactions from the week, including v0, v1-format and ALT-heavy
  transactions.

**Accept:**
- 100% exact match on every integer and hash feature, and on float features to the last bit.
- ALT resolution agrees with the recorded `resolved_keys` for every transaction whose tables are in the cache;
  cache misses are counted and reported, never silently zero-filled.
- Throughput and latency of the builder alone reported (target below).

### M2 — Inference on the hot path

- Port the bundle's LightGBM models to C. Code generation from the model text is fine; a runtime that parses the
  text file at start-up is fine. No dynamic allocation on the scoring path.
- Train a compact variant (distillation or fewer, shallower trees) with the same features, same train/val
  protocol, same thresholds procedure.

**Accept:**
- Full model: C scores equal the LightGBM reference to within 1e-9 on every transaction of the parity set.
- Compact model: on the offline test window, value captured at 1,000 decisions/h is at least 90% of the full
  model's; p99 scoring time per candidate transaction ≤ 1 µs on the production core.
- The pool lookup itself (touched pools → candidates exist?) reported separately; target p99 ≤ 300 ns.

### M3 — Shadow paper trader

- Wire it end to end on the live OrbitFlare feed (and DoubleZero / Blockspace when available):
  feed race → shred race → TXFRAME → ALT resolve → features → pool lookup → score → EV → threshold → top-L packs.
- Log every decision and a deterministic sample of non-decisions in the schema in
  [docs/05-paper-trading-spec.md](docs/05-paper-trading-spec.md), with `rx_tsc` stamps at every stage.
- Run continuously for at least 24 hours, then at least 7 days.

**Accept:**
- Decision rate within ±25% of the offline budget the threshold was fitted for (start with 1,000/h).
- Zero crashes; dropped or unparseable packets counted by cause.
- Score distribution on live transactions compared with offline (quantiles of P(arb), EV, share with candidates):
  report any shift larger than 10% at any decile.

### M3b — Landing head (P(land) gate)

Added after M3 found that only 19% of live decisions landed. Bundle `landing-v1`; full spec in
[docs/10-landing-head.md](docs/10-landing-head.md).

- Build its 35 static inputs and the causal fee-payer reputation in C, next to the arb features.
- Score it with the same LightGBM path as M2.
- Run policy C (`p_land ≥ 0.5`) next to your policy B (bad-payer gate), logging both.

**Accept:** the L1–L4 criteria in docs/10 must all pass:
- exact feature parity on the fixture window;
- model parity to 1e-9;
- exact reputation parity;
- a 24-hour shadow comparison of B against C.

### M4 — Score the paper trades

- Join the decision log to ground truth: (a) realised arbs on chain in the following transactions and slots
  (signer's base-token balance rose through the same pools); (b) labeller output for the same window once the
  labeller runs on it (coordinate with the owner); (c) the collector's runtime data for exact pre/post balances.
- Compute the same metrics as offline: share of decisions that were real arbs, value targeted per hour, value
  covered by the sent top-L routes, send mix.

**Accept:** a report comparing live with offline at the same budget, with confidence intervals, and a list of the
largest misses (high-value arbs we did not target) with a cause for each (no candidate, below threshold, feature
mismatch, not framed in time).

### M5 — Send-path dry run

- For every decision, build the real transaction with `arb-exec` (template, route family, nonce, Ed25519 sign),
  and stop before `leader_send`.
- Simulate a sample against recent state (LiteSVM or RPC `simulateTransaction` off-path) to check the bytes are
  valid and the route executes.

**Accept:**
- rx → signed bytes ≤ 1 ms p99, with each stage's share reported.
- ≥ 99% of built transactions pass simulation for routes the executor supports; the share of decisions whose
  routes the executor cannot express is reported per route family (this tells us what executor work is missing).

### M6 — Live canary (only with explicit approval)

Small, capped live sends on the best-scoring decisions, with a kill switch and a daily loss limit set by the
owner. Not to be started without written sign-off.

## Latency budget (targets)

| Stage | Target p99 |
|---|---|
| Feed packet → shred race decision | measured today in `the-firm` benchmarks; keep |
| Shred → framed transaction | measured (TXFRAME-001); keep |
| ALT resolve (cache hit) | ≤ 200 ns |
| Feature build | ≤ 1 µs |
| Pool lookup + candidate check | ≤ 300 ns |
| Score (compact model) | ≤ 1 µs |
| Pack selection + template patch + sign | ≤ 25 µs (Ed25519 ≈ 15 µs p50 today) |
| **Packet → signed bytes** | **≤ 1 ms p99; feature-to-decision ≤ 2 µs p50** |

Measure with `rdtscp` stamps carried on the item (`net_packet_t.rx_tsc` is already there); see
[docs/06-latency-eval.md](docs/06-latency-eval.md).

## How you will hear about new labels and models

The labeller is being rebuilt to cover all venues with better labels. When a new weekly run lands, a new bundle
with the same schema (`firm-pool-ev-bundle-v1`, or a documented v2) is published to Tigris. Your code should load
any bundle by path, and the evaluation should rerun unchanged against it.

## Contacts and decisions

The owner decides: retraining, thresholds or budgets beyond those in the bundle, anything that touches the
labeller or collector, and any live send. Raise decisions in the milestone reports.
