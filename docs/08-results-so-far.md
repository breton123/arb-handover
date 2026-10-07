# Results so far (offline, week of 2026-09-25)

All numbers are on held-out data from the week: validation (slots 452,133,789–452,435,054) for every choice, test
(452,435,055–452,736,205, 22.4 chain hours) reported once.

## The data

| | |
|---|---|
| Master transactions | 28,917,512 (41 partitions) |
| POSITIVE / CERTIFIED_NEGATIVE / UNKNOWN | 86,196 / 722,869 / 28,108,447 |
| Labelled universe | 3 venues (Raydium CLMM, Orca Whirlpool, Meteora DLMM), WSOL cycles of 2–5 hops |
| Encoder failures (framed-tx-v2) | 0 (22% of transactions are v1-format) |
| Test-window labelled opportunity | 0.221 SOL per chain hour (≈5.3 SOL/day), a lower bound |
| Concentration | top 1% of positives hold 76% of profit; median positive nets 14,190 lamports |

UNKNOWN splits into `NO_LABEL` (the transaction changed no modelled market; 3.39M in test), `UNRESOLVED` (route
search gave up; 1.0M) and `ENTRY_DEPENDENT` (0.1M). Treating NO_LABEL as negative is correct for this universe
and is what the bundle does.

## What moved the result

| Step | Test classifier PR-AUC | Value targeted at ~1,000 decisions/h |
|---|---|---|
| v1 notebook (count objective, library packs, UNKNOWN excluded) | 0.79 on labelled only | 0.2% |
| Value-weighted policy (EV = P × size) | same | 6.2% |
| NO_LABEL as negative | 0.27 (all relevant, base 0.33%) | 6.9% |
| Pool lookup for candidates | same | 5.7% |
| Pool lookup + pool priors + swap-size features (experiment) | 0.70 | 31.7% (≈0.070 SOL/h, net ≈0.058 SOL/h) |
| **Exported bundle `pool-ev-v1`** (same, every train-side pool feature out-of-fold) | **0.63** | **32.1%** (0.071 SOL/h, net 0.057 SOL/h) |

The bundle's full table is in [02-model-card.md](02-model-card.md) and its `eval.json`.

## Coverage of candidates

Share of test positive value covered by the top-k candidate routes:

| Routes considered | Pool lookup | Library per trigger key |
|---|---|---|
| 9 | 41.2% | 16.2% |
| 24 | 52.8% | 22.9% |
| 96 | 64.2% | 50.0% |

95% of test positives have a profitable route that was profitable somewhere in training, so coverage is limited by
selection, not by unseen routes.

## Where sends go

At ~1,000 decisions/h: about 7% of decisions are labelled positives, 13% NO_LABEL, 76% UNRESOLVED. UNRESOLVED
transactions look most arb-like to the model; their value is not counted anywhere above, so live scoring against
realised arbs (M4) may come out better than offline.

## Caveats you will meet live

- Labels are 3 venues, WSOL only, lower-bound profits. The rebuilt labeller (13+ venues, VM quoting, near-optimal
  sizing, realised-arb labels) will change the numbers; the bundle schema stays.
- Results are noisy: a few large arbs dominate. Val and test value differ by up to 3× at the same budget.
- Pack feasibility and tips are not modelled offline.

## Where it came from

- Notebook: `offline-ml/ml/notebooks/v1_week.ipynb` (executed copy on the box: `runs/v1_week_executed.ipynb`)
- Experiments: `offline-ml/ml/experiments/` — `value_and_unknown.py`, `nolabel_negative.py`, `extract_extras.py`,
  `pool_levers.py`, `export_bundle.py`, `venue_census.py`
- Outputs on the box: `runs/v1-week-20260925-run-001/experiments/*.json`
