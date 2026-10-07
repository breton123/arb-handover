# Model card: `pool-ev-v1`

Bundle: `ml/labeller-week-20260925-001/run-001/bundles/pool-ev-v1` (schema `firm-pool-ev-bundle-v1`), produced by
`offline-ml/ml/experiments/export_bundle.py`.

## What it does

For a raw Solana transaction (framed, ALT-resolved), it estimates:

- `p_arb`: the probability that the transaction creates a profitable WSOL arbitrage cycle (2–5 hops) on Raydium
  CLMM, Orca Whirlpool or Meteora DLMM;
- `size`: the expected log net profit in lamports, if it does.

`EV = p_arb × exp(size)` ranks transactions; a transaction is sent when it has candidate routes and EV clears the
threshold for the chosen send budget ([04-decision-policy.md](04-decision-policy.md)).

## Models

| | Arb classifier | Size model |
|---|---|---|
| Algorithm | LightGBM binary, 15 leaves, learning rate 0.03, early stopping on val PR-AUC | LightGBM regression (L2), same settings |
| Target | POSITIVE = 1; CERTIFIED_NEGATIVE and NO_LABEL = 0; UNRESOLVED and ENTRY_DEPENDENT excluded | log of best net profit (lamports), POSITIVE only |
| Weights | NO_LABEL subsampled 1 in 8 (by global id) with weight 8; others 1 | none |
| Inputs | 173: 159 `txflat-v2`, 8 raw extras, 6 pool priors ([03-feature-contract.md](03-feature-contract.md)) | same |
| Files | `arb_classifier.txt` (+ `.json`, `.encoder.json`) | `size_model.txt` (+ `.json`, `.encoder.json`) |

## Data and protocol

- Week of 2026-09-25: 41 partitions, 28.9M master transactions, labels from `labeller-week-20260925-001`.
- Split 70/15/15 by slot. Models, pool tables and vocabularies are fit on **train only**; early stopping, budgets
  and thresholds on **validation**; test is reported once.
- Pool priors and route counts for train rows are **out-of-fold** (5 contiguous slot folds), so no train row sees its
  own label. Validation, test and the bundle tables use all of train.

## Quality (held-out)

| | Validation | Test |
|---|---|---|
| Base rate (positives among labelled, gate-free) | 0.26% | 0.34% |
| Arb classifier PR-AUC | 0.606 | **0.629** |
| Arb classifier ROC-AUC | 0.993 | 0.994 |
| Size model rank correlation with true profit | 0.572 | 0.510 |

## Economics on the test window (22.4 chain hours)

Labelled opportunity: **0.221 SOL per hour** (all positives, lower-bound profit). Captured = a chosen route is
profitable for that transaction. Net subtracts 5,000 lamports per transaction sent (L per decision).

| Budget (decisions/h) | L | Test decisions/h | Positives captured | SOL/h captured | Share of opportunity | Net SOL/h |
|---|---|---|---|---|---|---|
| 30 | 2 | 24 | 21 | 0.0044 | 2.0% | 0.0042 |
| 100 | 3 | 90 | 95 | 0.0265 | 12.0% | 0.0252 |
| 250 | 3 | 219 | 217 | 0.0507 | 22.9% | 0.0474 |
| **1,000** | **3** | **951** | **614** | **0.0711** | **32.1%** | **0.0569** |
| 4,000 | 3 | 4,483 | 1,545 | 0.0786 | 35.5% | 0.0114 |
| 16,000 | 3 | 17,669 | 2,483 | 0.0891 | 40.3% | −0.1759 |

Full grid, validation numbers and send mix per budget: `eval.json`.

## Intended use and limits

- **Use:** shadow paper trading and hot-path evaluation on mainnet traffic shaped like the training week.
- **Universe:** three venues and WSOL bases only. Transactions that create arbs elsewhere score low by design.
- **Labels are lower bounds** (≤ 4 sizing probes, 10 SOL capital, flat 15,000-lamport route cost); UNRESOLVED
  transactions (most of what gets sent) carry unknown value.
- **Drift:** pool tables come from one week. Expect decay as pools and flows change; the weekly pipeline refreshes
  them.
- **Inputs:** ALT-resolved account lists. Live, that needs the LUT cache; a miss means not scored.
- **Not a sizing model for execution:** trade size is chosen on chain by the executor at execution time.
