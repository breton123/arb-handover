# offline-ml

Snapshot of the offline research pipeline (from `the-firm-offline`, 2026-10-07). It is the reference implementation
for every feature and decision the paper trader must reproduce.

| Path | What |
|---|---|
| `crates/tx-features` | framed-tx encoder (Rust, with a C ABI): `encode_transaction_v2` handles legacy, v0 and v1 (`0x81`) transactions |
| `crates/tx-features-cli` | command-line encoder |
| `docs/ml/` | feature specs (`FRAMED_TX_FEATURES_V1.md`, `V2.md`) and encoder benchmarks |
| `ml/prep` | `ml-prep` (Rust): raw transactions + labels + candidate library → per-partition training tables (`txflat-v2` flattening in `src/flatten.rs`) |
| `ml/python/firm_ml` | Python package: loaders (`data.py`, `week.py` for a whole week), splits, encoding, models, metrics, policies |
| `ml/notebooks/v1_week.ipynb` | the weekly research notebook |
| `ml/experiments/` | the experiments behind the current model, and `export_bundle.py` which produces the paper-trading bundle |
| `ml/weekly/` | fleet worker and orchestrator for the weekly run (`orchestrate.py` expects the labeller repo's `fleet/` package and Cherry credentials; you only need `fetch.py`) |
| `raw-tx-snippet.jsonl` | small raw-transaction corpus used by the encoder tests (the 100k corpus is not included) |

## Build and test

```bash
# Rust: encoder, ml-prep
cargo test --release -p tx-features
cargo test --release -p ml-prep

# Python
cd ml
uv sync
uv run pytest -q python/tests        # data tests skip unless FIRM_ML_PREPARED points at a prepared partition
```

## Reproduce the bundle

On the Frankfurt box, with the week fetched under `/data/bsc/captures/firm-ml/week/run-001` and the raw extras
extracted (`experiments/extract_extras.py`):

```bash
cd ml
uv run --with boto3 python experiments/export_bundle.py \
  --week /data/bsc/captures/firm-ml/week/run-001 \
  --run /data/bsc/captures/firm-ml/runs/v1-week-20260925-run-001 \
  --credentials tigris.json --out ./bundle
```
