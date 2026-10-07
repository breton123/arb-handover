# Leakage audit of the ML pipeline

Scope: everything after the labeller (features, tables, splits, models, thresholds, evaluation) for bundle
`pool-ev-v1`. The labeller's labels are assumed leakage free. Script:
`offline-ml/ml/experiments/leakage_audit.py`; results: `runs/v1-week-20260925-run-001/experiments/leakage_audit.json`
on the Frankfurt box.

## Verdict

No leakage was found. The test numbers are honest estimates for this week and this labelled universe, with the
measurement caveats below. The model's skill comes mostly from **which pools produce arbs**, learned from the
training window, and it **decays with staleness**, so live results will depend on how often it is retrained.

## Code review

| Check | Result |
|---|---|
| Inputs come only from the raw transaction | Yes. `txflat-v2` is computed from wire bytes; the raw extras use static keys, ALT-resolved keys, message writability and instruction data. The dataset's transaction record has no runtime fields by construction. |
| Ids, time and labels refused as inputs | Yes. `slot`, `signature`, `block_time`, `entry_index`, `tx_index` are id/meta columns; no blockhash or signature bytes are features (the "raw" group is instruction-data prefixes). |
| Hash vocabularies | Fit on train rows only. |
| Pool priors and route tables | Built from train only. Train rows get out-of-fold values (5 contiguous slot folds), including route counts (fixed in the bundle; the earlier `pool_levers.py` experiment had in-sample route counts on train rows, which is why it reported PR-AUC 0.70 against the bundle's 0.63). |
| Splits | 70/15/15 by slot, in time order, no overlapping transactions across partitions. |
| Model selection | Early stopping, live top-L and budget thresholds fit on validation. Test reported once per configuration. |
| NO_LABEL subsample | `global_id % 8`, independent of the label. |
| Evaluation | Candidate routes come from train tables; test labels are used only to score hits. |

## Empirical tests

| Test | What a leak would look like | Result |
|---|---|---|
| Baseline rebuild | — | Reproduces the bundle exactly: test PR-AUC 0.629, ROC-AUC 0.994, 32.2% of value at 1,000 decisions/h |
| Shuffled labels (same rows, features, weights) | Skill above chance | Test PR-AUC 0.0034 = base rate, ROC 0.49 |
| Single-feature screen | An id-like or label-derived input that nearly solves the task | Top inputs are the train-derived pool priors (AUC 0.96–0.97 alone); best raw-transaction input 0.67. Strong but legitimate: arbs recur on the same pools. |
| Pool universe frozen at train end | Drop if knowing test-window pools helped | No change (PR-AUC 0.6290 vs 0.6289; identical capture) |
| Memorisation | Skill carried by train duplicates | 24% of test rows repeat a train feature vector (bots), but only 3.4% of positives; PR-AUC without duplicates 0.630 |
| Unseen pools | — | Only 0.24% of test positives (3% of value) touch no pool seen in train: the model is rarely tested on new pools |
| Embargo (drop the last 15% of train, ~18 chain hours, from every fit and table) | A cliff at the train/test boundary | PR-AUC 0.629 → 0.554; value at 1,000/h 32.2% → 26.1%. The drop is spread across the test window rather than concentrated next to train, so it reads as staleness, not a boundary leak |
| Time decay within test (quarters, 1,000/h) | Best quarter right after train | PR-AUC 0.63, 0.62, 0.55, 0.77; value share 12%, 53%, 27%, 27% |

## What still limits trust (measurement, not leakage)

1. **Test reuse during research.** The experiments looked at test numbers before the final configuration was
   chosen. The choice is also the clear winner on validation (validation value captured 0.25 vs 0.06 SOL/h for the
   base features), but a pristine holdout is still needed: score the bundle unchanged on the 5 unlabelled partitions
   once labelled, or on next week.
2. **Heavy tail.** A few large arbs dominate value. At the same budget, validation captured 42% and test 32%;
   test quarters range from 12% to 53%. Quote value with intervals, not points.
3. **Value credit is optimistic.** A hit credits the transaction's best route profit when any chosen route is
   profitable, even if that route earns less. Per-route profits (coming from the labeller's `tx_routes`) fix this.
4. **No pack feasibility, priority fees or tips** in offline value; 5,000 lamports per transaction only.
5. **UNRESOLVED sends count as worthless** (most sends go there), and profits are lower bounds. These two bias the
   result down.
6. **Staleness.** An 18-hour-staler model loses ~12% PR-AUC and ~20% of captured value. Plan for daily retraining.
7. **Live coverage.** Inputs need ALT resolution; the hot path's LUT cache resolved 89.8% of eligible transactions
   in earlier measurements. Unresolved transactions are not scored.
