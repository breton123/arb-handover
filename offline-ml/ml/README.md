# ml: V1 decision stack against a frozen candidate library

```text
raw tx -> encode_transaction_v2 (framed-tx-v2) -> relevance gate (relevance-v1) -> flatten_v2 (txflat-v2)
       -> arb probability -> candidate-library lookup (depth k) -> pack ranking -> send top 1-3
```

**Main question:** given a raw transaction and a frozen, training-built candidate library, can we pick a small number
of pack sends with a meaningfully higher probability of capturing an out-of-sample arb?

**Evaluation protocol.**
1. Split slots 70 / 15 / 15.
2. Build the candidate library (`candidate-packs build-library`) from **train** master ids only, and freeze it.
3. Train the arb classifier and the pack ranker on train.
4. On validation, choose the thresholds (per send budget), the library depth k, the live top-L, and whether to keep the learned ranker.
5. Evaluate on test once.

The notebook refuses a prepared dataset whose catalog was built outside the train split.

| path | what |
|---|---|
| `prep/` | Rust `ml-prep`: raw txs + economics labels + positives index + pack catalog -> ML Parquet. Workspace member |
| `prep/src/flatten.rs` | `txflat-v2` fixed-width row from `EncodedTxV2` (159 columns: framed-tx header, aggregates, v1-format config, 8 instruction slots). No allocation; live inference calls the same function |
| `prep/src/relevance.rs` | `relevance-v1` gate: a tracked market account or venue program is a static key of the raw tx |
| `prep/src/packs.rs` | reader for `candidate-library-v1` (`build-library`) and legacy `candidate-pack-v1` (`build`) catalogs |
| `prep/src/pairs.rs` | `(tx, pack)` expansion up to depth k, after the relevance gate is known |
| `python/firm_ml/` | loaders, slot splits, encoders, LightGBM training/persistence, metrics, `policy.py` (decision rules), plots |
| `notebooks/v1_research.ipynb` | the research flow |
| `PLAN.md` | design notes and leakage boundaries |

## 1. Candidate library (from the candidate-pack repo)

`ml` does not duplicate any candidate-pack logic. It reads the library repo's artifacts:
- `library_packs.parquet` and `library.json`, from `build-library`;
- the positives index, `positives.parquet` and `library-index.json`, from `index-library`.

The index is the same source of profitable RouteIds that `build-library` and `sweep-library` use. The notebook asserts
that `ml-prep`'s availability equals `sweep-library`'s `positive_tx_coverage` at every k.

On the dev box, build `candidate-packs` from `firm-dataset-labeller` with `--offline`, then run:

```bash
# dev box: louis@195.242.152.178; all build output under /data/bsc (the root disk is full)
B=/data/bsc/captures/sol-dataset-446457211-20260927
R=$B/ml-pack-speed-pipeline-054/labelling/labels      # economics labels (label --mode ml)
L=$B/ml-library-054-001
CP=/data/bsc/captures/ml-prep-build/labeller-target/release/candidate-packs
# library-train.json = config/library-v1.json with train_master_tx_id_min=1, train_master_tx_id_max=<last train master>
$CP index-library --economics $R --output $L/index
$CP build-library --economics $L/index --output $L/library --config $L/library-train.json
$CP sweep-library --library $L/library --economics $L/index --master-min <val min> --master-max <val max> --ks 1,2,3,5,8,16,32,64 --output $L/sweep-library-val.json
# and the same for test, and with --library <legacy packs dir> as sweep-legacy-{val,test}.json
```

Take the master-id windows from the slot split. For run 054 they are: train 1..972055, val 972056..1175132,
test 1175133..1367652. The notebook prints them as `master_id_windows`.

## 2. Prepare (Rust)

```bash
export CARGO_HOME=/data/bsc/captures/ml-prep-build/cargo-home CARGO_TARGET_DIR=/data/bsc/captures/ml-prep-build/target
cargo build --release --offline --locked -p ml-prep
ml-prep build --labels $R --positives $L/index --packs $L/library --raw-entries $B/car \
  --out $B/ml-prep-054-003 --slots-per-part 250 --threads 16 --max-depth 64 --unknown-pair-ppm 125000 \
  --dataset-id sol-dataset-446457211-20260927/run-054/library \
  --provenance $L/library-train.json --provenance $L/sweep-library-val.json --provenance $L/sweep-library-test.json \
  --provenance $L/sweep-legacy-val.json --provenance $L/sweep-legacy-test.json
# the legacy catalog, scored by the same code: --packs $B/ml-packs-train-054-001/packs --out $B/ml-prep-054-004
```

The 10k build takes 83 s for the library (4.0M pairs) and 34 s for legacy. Passes:
1. **labels** streams `trigger_occurrences`.
2. **features** runs per part, in parallel: raw → encoder → `txflat-v1` + relevance.
3. **pairs** runs per part, in parallel. It expands eligible packs with catalog rank < `--max-depth`:
   - POSITIVE: all layers.
   - CERTIFIED_NEGATIVE: live (TOP_LEVEL-key) packs.
   - UNKNOWN: live packs, only when the transaction is relevant and in the deterministic `--unknown-pair-ppm` sample.
     Python reweights these by 1/rate to estimate live volume.

For the weekly dataset, use the same commands and lower `--unknown-pair-ppm`. `--resume` keeps finished parts and
refuses to resume if any input hash or config changed.

Raw sources:
- `--raw-entries`: Old Faithful `{slot}.entries.json.zst` (the 10k run).
- `--raw-dataset`: a fleet dataset partition's `dataset/` directory. Only `compaction.json` and
  `part-*.transactions.parquet` are needed; `raw` is read from each `record_cbor`. Block times come from the labels'
  `labels.parquet`.
- `--raw-jsonl`: development only.

### Labelled fleet partitions from Tigris

Each published partition of `labels/labeller-week-20260925-001` has a source dataset under
`campaigns/.../partitions/<id>`. For each partition:

1. Fetch its labels and index with `firm-dataset-labeller/scripts/library_inputs.py`.
2. Fetch only its transactions parts: about 36 GB per 50k slots, SHA-256 verified, about 11 MB/s here.
3. Run:

```bash
ml-prep build --labels <labels> --positives <index> --packs <library> --raw-dataset <dataset dir> ...
```

### Outputs

| file | rows | contents |
|---|---|---|
| `tx_labels/part-*.parquet` | one per master tx | ids, `arb_label`, `arb_target` (1 / 0 / **null for UNKNOWN**), `trigger_keys` (→ `keys.parquet`), eligible/covering counts, `min_cover_rank_{live,oracle}` (covered at depth k ⇔ < k), `n_profitable_routes_seen_in_train`, `profitable_routes`, `pair_sample` |
| `tx_features/part-*.parquet` | same rows, same order | `master_tx_id, slot, block_time, encode_status`, the gate columns, then 159 `txflat-v2` features. **No label columns** |
| `pairs/part-*.parquet` | one per (tx, eligible pack) | `pack_id`, `pack_rank`, `eligible_live`, `pack_target` (pack RouteIds ∩ profitable RouteIds ≠ ∅; null for UNKNOWN) |
| `packs/pack_features.parquet` | one per pack | `packflat-v2` (library) or `packflat-v1` (legacy). Groups: `static` (structure and resources) and, for the library, `train_meta` (rank, selection gain, train coverage, novelty, route support), all from the library's training window |
| `keys.parquet` | one per trigger key | `(program_id, family_id, layer, base)` |
| `feature_manifest.json`, `manifest.json` | | ordered feature lists with groups; id/label/meta/gate columns; input and output hashes; library manifest; pack training window |

The build fails if any of these hold:
- a raw transaction's signature differs from the label's;
- a master transaction is missing from the raw source;
- the positives index disagrees with the POSITIVE trigger rows.

## 3. Research (Python)

```bash
cd ml && uv sync                 # Python 3.13, polars, pyarrow, numpy, scikit-learn, lightgbm, matplotlib, jupyter
FIRM_ML_PREPARED=<library build> FIRM_ML_LEGACY=<legacy build> \
  uv run jupyter nbconvert --to notebook --execute --inplace notebooks/v1_research.ipynb
```

On the dev box, put uv's cache, interpreter and venv under `/data/bsc`: `UV_CACHE_DIR`, `UV_PYTHON_INSTALL_DIR` and
`UV_PROJECT_ENVIRONMENT` in `/data/bsc/captures/ml-prep-build/py`. Set `FIRM_ML_RUNS` to choose where artifacts go.

Notebook sections:
1. Data and protocol checks
2. Classifier quality
3. Library availability vs k, with the `sweep-library` cross-check
4. Route-recurrence ceiling
5. Static vs learned pack ranking, with the keep rule
6. Two-stage vs joint decisions at validation-fitted send budgets
7. Failure decomposition and conditional results
8. Artifacts

Artifacts go to `<runs>/<experiment>/`:
- `models/{arb_classifier,pack_ranker}.txt`, with `.json` sidecars (feature order, params, protocol windows) and `.encoder.json` files.
- `operating_points.json`: per policy and budget, the depth, L and threshold.
- `metrics.json`, `predictions/*.parquet`, `plots/*.png`.
- `experiment_manifest.json`: input, output and artifact hashes.

### Implementing inference in Rust later

1. Run `encode_transaction_v2` (framed-tx-v2: legacy, v0 and the v1 `0x81` format; see `docs/ml/FRAMED_TX_FEATURES_V2.md`), then `TrackedUniverse::relevance` on `enc.base`. Drop the transaction unless it is relevant.
2. Run `flatten_v2` to get the tx features in `feature_manifest.json` order.
3. Encode `hash` columns through `<model>.encoder.json` (NaN when unseen). Pass `numeric` and `categorical` columns through as f64.
4. Look up the eligible library packs: the transaction's TOP_LEVEL keys, rank < k. Append each pack's `pack_features` row.
5. Evaluate the LightGBM text models. Apply the operating point from `operating_points.json`.

## Tests

```bash
cargo test --release -p ml-prep   # encoder parity; UNKNOWN never negative; RouteId pack targets; depth limit; UNKNOWN pairs
                                  # need gate + sample; positives-index mismatch fails; library and legacy give identical
                                  # targets; determinism + resume; no labels in feature files; relevance reads only static keys
cd ml && uv run pytest -q         # loaders keep feature order and targets; id/label/meta/gate refused as features; frozen
                                  # library window; slot splits; encoder/model round trips; policy accounting; encoder-CLI parity
```

The data-dependent Python tests read `FIRM_ML_PREPARED` and `FIRM_ML_DEV`, and skip when those datasets are absent.
