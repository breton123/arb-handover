# Data and infrastructure

## Object store (Tigris, S3-compatible)

Credentials are `TIGRIS_URL_S3`, `TIGRIS_REGION`, `TIGRIS_ACCESS_KEY_ID`, `TIGRIS_SECRET_ACCESS_KEY`,
`TIGRIS_BUCKET`, as environment variables or a JSON file passed with `--credentials`. Ask the owner; never commit
them.

Every published output follows the same convention: files under `<prefix>/attempts/<uuid>/`, an immutable
`publication.json`, and a conditional `<prefix>/COMPLETE` marker. Read only prefixes that have `COMPLETE`, and verify
every file against its manifest sha256. `offline-ml/ml/weekly/fetch.py` does this.

| Prefix | What |
|---|---|
| `ml/labeller-week-20260925-001/run-001/PLAN.json` | the 41 partitions of the week, slot ranges, raw source prefixes |
| `…/run-001/index/<partition>` | candidate-library index per partition, plus the label tables the ML pipeline reads |
| `…/run-001/library` | the frozen candidate library (train window only), windows, val/test sweeps |
| `…/run-001/prep/<partition>` | `ml-prep` output per partition: tx features, labels, transaction–pack pairs |
| `…/run-001/bundles/pool-ev-v1` | **the paper-trading model bundle** |
| `campaigns/firedancer-week-20260925-stream-003-p000/partitions/<partition>` | raw transactions (`dataset/part-*.transactions.parquet`, CBOR records with resolved keys) and runtime data |

Fetch commands (from `offline-ml/ml`):

```bash
# the model bundle only (a few hundred MB at most)
uv run --with boto3 python weekly/fetch.py --out ./data --credentials tigris.json --bundle pool-ev-v1

# the prepared week (≈15 GB: library + 41 prepared partitions)
uv run --with boto3 python weekly/fetch.py --out ./data/week/run-001 --credentials tigris.json
```

Raw transactions are about 34 GB per partition (1.39 TB for the week). Stream them instead of storing them:
`offline-ml/ml/experiments/extract_extras.py` shows the pattern (download a part, read it from memory, delete).

## The week

- Labels run `labeller-week-20260925-001`, ML run `run-001`.
- 41 of 46 partitions labelled, slots 450,478,612 to 452,736,205 (≈2.0M slots, ≈28.9M master transactions).
- Split 70/15/15 by slot. Train ends at slot 452,133,788; val is 452,133,789–452,435,054; test is
  452,435,055–452,736,205 (22.4 chain hours).
- Global transaction id: `partition_index << 40 | master_tx_id` (`firm_ml.week` re-keys every row).

## The Frankfurt box

`louis@195.242.152.178` (1 TB RAM, 25 cores; also runs a validator and the capture stack, so leave headroom).

| Path | What |
|---|---|
| `/data/bsc/captures/firm-ml/code` | a working copy of `offline-ml` (uv environment in `code/.venv`) |
| `/data/bsc/captures/firm-ml/week/run-001` | the fetched week: `library/`, `prep/<partition>/`, `extras/<partition>.parquet`, `labels/` (test-window trigger tables), `indexes/`, `markets/` |
| `/data/bsc/captures/firm-ml/runs/v1-week-20260925-run-001` | the weekly notebook run: `metrics.json`, `models/`, `predictions/`, `plots/`, `experiments/` |
| `/data/bsc/captures/firm-ml/bundles/pool-ev-v1` | the exported bundle (also on Tigris) |
| `/data/bsc/captures/firm-ml/bin/uv` | uv; use `UV_CACHE_DIR=/data/bsc/captures/firm-ml/uv-cache` (the root disk is full) |
| `/data/bsc/captures/orbitflare.env` | OrbitFlare credentials (owner only) |
| `~/the-firm`, `~/arb-exec`, … | the original hot-path trees this repo snapshots |
| `/home/louis/fixtures/orbitflare-20260927-002844.cap` | frozen OrbitFlare capture used by TXFRAME tests |

Disk: `/` is full and `/data/bsc` has little headroom. Keep large data under `/data/bsc/captures` and stream raw
data rather than storing it.

## The bundle

`firm-pool-ev-bundle-v1`, produced by `offline-ml/ml/experiments/export_bundle.py`:

| File | Content |
|---|---|
| `manifest.json` | schema, label universe, target definition, windows, feature order and groups, file list |
| `arb_classifier.txt`, `.json`, `.encoder.json` | LightGBM text model, metadata (feature order, params), hash vocabularies |
| `size_model.txt`, `.json`, `.encoder.json` | same for the size model (log lamports of net profit) |
| `pool_stats.parquet` | `pool, touch, pos, pos_rate, logprofit, n_routes` (train window) |
| `pool_routes.parquet` | `pool, route, support, rank` (top 192 routes per pool by train support) |
| `routes.parquet` | `route_id, market_ids (ordered pools), hops` for every route in `pool_routes` |
| `thresholds.json` | per budget: `ev_threshold`, `L`, `routes_per_decision` |
| `eval.json` | val/test quality, the full budget × L grid, the chosen operating points, test opportunity per hour |
