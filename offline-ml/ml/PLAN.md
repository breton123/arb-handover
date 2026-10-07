# ML V1 — implementation plan

Status: implemented for the 10k-slot run 054, with the relevance gate and train-only frozen packs
(`ml-prep-054-002`). The weekly dataset uses the same commands.

## What already exists (inspected, not reinvented)

| input | where | key facts |
|---|---|---|
| feature encoder | `crates/tx-features` `encode_transaction_v1` (framed-tx-v1) | the only feature implementation. Header + per-instruction + per-account records |
| raw transactions | dev box `…/car/{slot}.entries.json.zst` (Old Faithful entries, txs as byte arrays); `raw-tx-*.jsonl` (`slot, tx_index, signature, raw_hex`) locally | `tx_index` is the slot-global index across entries |
| trigger labels | run 054 `labelling/labels/trigger_occurrences.parquet` (`trigger-occurrences-1`) | one row per (call × base); sorted by `master_tx_id`; status is per-tx consistent |
| pack labels | run 054 `pack-run/packs/occurrence_pack_labels.parquet` | same rows; `profitable_route_ids`, `covering_pack_ids` |
| packs | `candidate_packs.parquet`, `route_pack_membership.parquet` | trigger key `(program_id, family_id, layer, base)`; covers = route sets intersect |

`master_tx_id` is tape-local. It exists only for committed transactions that wrote to a venue dependency account. It is dense, starts at 1 and is monotonic in `(slot, tx_index)`. The join to raw bytes is `(slot, tx_index)`, verified against the first signature.

## Leakage boundaries found during inspection

- `INNER` and `ATTRIBUTED` trigger rows come from runtime CPI metadata and the best route. They are not observable before execution. Pack eligibility is therefore evaluated two ways. **live** uses TOP_LEVEL keys only and is the headline number. **oracle** uses all layers and is the upper bound.
- `candidate_packs` were built with `train_master_tx_id_min=0..u64::MAX`, i.e. in-sample on the full window. Every coverage/objective/penalty `*_ppm`, `objective_score`, `positive_*`, `value_*` and `rank` column is future-derived. None is a ranker feature. `rank` is kept only as the `builder_rank` baseline. 
  - **Resolved:** packs are rebuilt on the train window (`ml-packs-train-054-001`) and frozen. Val and test pack labels come from `candidate-packs evaluate`. The notebook asserts the pack window lies inside the train split.
- Full-stream send volume was dominated by `NO_LABEL` transactions, which touch no tracked market and which the classifier never trains on.
  - **Resolved:** the deterministic `relevance-v1` gate runs before the model. `NO_LABEL` is not relabelled.
- `family_pack_policy.positive_prior_ppm` is future-derived and is not used.
- The master-tx population is conditioned on committed venue writes, so failed transactions are absent.

## Rust: `ml/prep` (`ml-prep build`)

One streaming pass over the label tables, then a per-slot-part parallel pass over the raw transactions.

1. **labels pass**: merge-walk `trigger_occurrences` + `occurrence_pack_labels` grouped by `master_tx_id`. Both are sorted, so memory is O(one tx).
   - Writes `tx_labels/part-*.parquet`, one row per master tx: identity, `arb_label`, `arb_target` (1 / 0 / null), occurrence counts per layer, eligible/covering pack counts.
   - Writes `pairs/part-*.parquet`, one row per (labelled tx, eligible pack): `pack_target`, `eligible_live`, `eligible_oracle`.
   - Recomputes coverage from RouteIds and asserts it equals the labeller's `covering_pack_ids`.
2. **features pass**: for each slot part, read the raw txs, merge-join the master txs on `(slot, tx_index)`, check the signature, call `encode_transaction_v1`, then flatten to a fixed-width row (`txflat-v1`, `ml/prep/src/flatten.rs`, which live inference can reuse). Writes `tx_features/part-*.parquet`. **The feature files contain no label columns.**
3. **packs**: writes `packs/pack_features.parquet` with static, safe pack metadata: route/hop structure, venue family counts, accounts, writable, dynamic groups, CU, message bytes.
4. Writes `feature_manifest.json`, which holds the ordered feature lists with dtype, group and kind (`numeric` | `hash` | `categorical`), plus `manifest.json`, which holds input sha256s, row counts, schema versions, output sha256s and the config.

Parts are keyed by `slot / slots_per_part`. They are written to a temp file and renamed into place. With `--resume`, a part is skipped when it already exists and its recorded input digest matches. Output is deterministic: sorted rows, fixed row-group sizes, no timestamps in the data files.

## Python: `ml/python/firm_ml` + `ml/notebooks/v1_research.ipynb`

- `data.py`: load the prepared parts. Feature columns come only from `feature_manifest.json` (an allowlist), and the loader refuses ID/label columns.
- `splits.py`: slot-ordered 70/15/15 boundaries over distinct slots. Each tx has exactly one slot, so it lands in exactly one split.
- `encoding.py`: hash columns become train-fitted vocabularies (top-N, `-1` = unseen), saved for Rust.
- `metrics.py`: PR/ROC, threshold tables, top-k / MRR, and the combined funnel.
- `models.py`: LightGBM training and persistence (text model + JSON sidecar with the feature order).
- Notebook: validation → split → arb classifier (unweighted LightGBM, early stopping on val PR-AUC, thresholds picked on val by precision target) → pack ranker (binary LightGBM on eligible pairs of positive txs) → combined OOS funnel → artifacts.

## Candidate library integration (`candidate-library-v1`)

- **Catalog.** `ml-prep --packs` reads a `build-library` directory or a legacy `build` directory. Both are scored by the
  same code. Profitable RouteIds come from the library repo's own positives index (`index-library`).
- **Cross-check.** The notebook asserts that availability at every k equals `sweep-library`'s `positive_tx_coverage`.
- **Library metadata as features.** Library ranking metadata (rank, selection gain, train coverage, novelty, route
  support) is a `train_meta` pack feature group. It is valid because the library is built from train master ids only,
  and the notebook asserts that window ends inside the train split.
- **Pairs pass.** Pairs are produced by a separate per-part pass after the relevance gate, up to `--max-depth`:
  - POSITIVE: all layers.
  - CERTIFIED_NEGATIVE: live packs.
  - Relevant UNKNOWN: live packs, on a deterministic sample, reweighted in Python. This bounds pair volume for the
    weekly run.
- **Policies.** `firm_ml/policy.py` compares two-stage (arb threshold → rank packs) with joint (max P(arb)·P(pack)
  ≥ t) at send budgets fitted on validation. Each positive gets one outcome, in pipeline order: gate → threshold →
  no covering pack → ranking.
- **Fleet partitions.** `--raw-dataset` reads the dataset partition format: `compaction.json` plus `part-*.transactions.parquet`, with `raw` taken from `record_cbor`. It is tested byte-identical to the JSONL source.

## framed-tx-v2 (encoder)

- **Why.** In p045, 33% of master transactions (25% of positives) are v1-format transactions (`0x81`), which framed-tx-v1 cannot parse.
- **What v2 is.** `encode_transaction_v2` (`crates/tx-features`) frames the v1 format and feeds the same feature derivation. It reports the message-level config (priority fee in lamports, CU limit, loaded-data limit, heap) in `TxExtV2`.
- **Erratum fix.** v2 also fixes a framed-tx-v1 erratum. v1 expands lookup-loaded rows table by table instead of in runtime order (all writable, then all readonly); that's wrong for transactions with two or more tables (~65% of p045). framed-tx-v1 itself is unchanged and frozen.
- **Spec.** `docs/ml/FRAMED_TX_FEATURES_V2.md`.
- **Validation.** An independent reference, the 100k corpus, and all 32.7M p045 transactions checked against the collector's own decoding.
- **Datasets.** `ml-prep` now emits `txflat-v2`. Datasets built before this (`ml-prep-054-*`, `p045-*`) are txflat-v1; rebuild them to compare.
