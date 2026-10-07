# Feature contract

The model takes **159 + 8 + 6 = 173 inputs** per transaction, in the exact order of `feature_order` in the bundle's
`arb_classifier.json` (the same order for `size_model.json`). Every input is computed from:

1. the framed raw transaction (wire bytes, after TXFRAME);
2. its ALT-resolved account list (static keys, then all writable lookup addresses, then all readonly, in
   descriptor order: the canonical v0 order already implemented in `the-firm` `txview_resolve`);
3. the bundle's offline tables (`pool_stats.parquet`, `pool_routes.parquet`).

No account data, balances, pool reserves, logs or execution results. If a value cannot be computed (ALT miss),
the transaction is not scored; count it.

## Group A — `txflat-v2` (159 inputs)

The framed-tx-v2 encoder plus the `txflat-v2` flattening. The authoritative definitions are:

- Spec: `offline-ml/docs/ml/FRAMED_TX_FEATURES_V2.md` (and the erratum in `FRAMED_TX_FEATURES_V1.md`)
- Encoder: `offline-ml/crates/tx-features` — `encode_transaction_v2(raw) -> EncodedTxV2 { base, ext }`, with a C ABI
  (`txf_encode_v2`, header `include/tx_features.h`, layout checks in `abi_check.c`)
- Flattening to columns: `offline-ml/ml/prep/src/flatten.rs` (`flatten_v2`)
- The list, with dtype, kind and group for each column: [`feature_manifest.json`](feature_manifest.json) →
  `tx_features`

Composition: 134 structural (header, sizes, key counts, instruction slots 0–7, fingerprints), 16 raw (selected
byte prefixes and lengths), 9 identity (program hashes). Kinds: 123 numeric, 9 categorical, 27 hash.

Handling by kind (from `offline-ml/ml/python/firm_ml/encoding.py`):

| Kind | Model input |
|---|---|
| numeric | the stored integer as float64 |
| categorical | the stored small code, treated as categorical by LightGBM |
| hash | 64-bit hash → code `0..k-1` through the vocabulary in `*.encoder.json` (`vocab[column][str(hash)]`); unseen or rare hash → missing (NaN) |

The encoder already has a C ABI. The fastest correct M1 path is to link it (`crates/tx-features` builds a static
library) and port only `flatten_v2` and the vocabulary lookup to C. A pure C port is acceptable if parity is exact.

Note: v1-format transactions (`0x81` prefix) are 22% of the week and must be supported; framed-tx-v2 handles them,
and TXFRAME in `the-firm` already frames them.

## Group B — raw extras (8 inputs)

Reference implementation: `offline-ml/ml/experiments/extract_extras.py` (`extract`) and the column transforms in
`pool_levers.py` / `export_bundle.py`.

Definitions. *Pool* means a pool of a labelled venue: the union of `pool_stats.parquet` pools and
`pool_routes.parquet` pools is a superset of what matters; the offline reference used the full list of markets of
the three venues (20,762 pools for the week).

| Input | Definition |
|---|---|
| `n_pools_all` | number of distinct pools among all resolved keys |
| `n_pools_static` | number of distinct pools among the static keys |
| `n_pools_writable` | number of distinct pools whose resolved key is writable in the message |
| `n_venue_swaps` | number of top-level instructions whose program is Orca Whirlpool, Raydium CLMM or Meteora DLMM and whose data starts with an Anchor discriminator of a swap (`sha256("global:<name>")[:8]` for `swap`, `swap_v2`, `swap2`, `swap_exact_out`, `swap_exact_out2`, `swap_with_price_impact`, `swap_with_price_impact2`, `two_hop_swap`, `two_hop_swap_v2`, `swap_router_base_in`) |
| `log_venue_swap` | `log1p` of the largest little-endian u64 at data offset 8 among those swap instructions (0 if none) |
| `n_jup_routes` | number of top-level Jupiter v6 instructions (`JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4`) with data length ≥ 27 |
| `log_jup_in` | `log1p` of the largest little-endian u64 at offset `len(data) − 19` among those (the route `in_amount`) |
| `wsol_in_keys` | 1 if `So11111111111111111111111111111111111111112` is among the resolved keys, else 0 |

Program ids:

- Orca Whirlpool `whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc`
- Raydium CLMM `CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK`
- Meteora DLMM `LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo`

## Group C — pool priors (6 inputs)

Lookups into `pool_stats.parquet` (columns `pool`, `touch`, `pos`, `pos_rate`, `logprofit`, `n_routes`, computed on
the training window only). For each pool among the **resolved keys** (the same set as `n_pools_all`):

| Input | Definition (missing pool contributes nothing; empty set gives 0) |
|---|---|
| `prior_touch_max` | max `touch` |
| `prior_pos_rate_max` | max `pos_rate` (= (pos + 0.1) / (touch + 10)) |
| `prior_pos_sum` | sum of `pos` |
| `prior_logprofit_max` | max `logprofit` (mean log lamports of positives through the pool; null counts as 0) |
| `prior_routes_max` | max `n_routes` |
| `n_candidate_routes` | sum of `n_routes` |

For the hot path: precompute a perfect hash from pool pubkey to a 24-byte record and do one probe per resolved key.

## Parity harness (M1 acceptance)

- Inputs: transactions from the week's raw dataset (Tigris; see [07-data-and-infra.md](07-data-and-infra.md)),
  at least 1,000,000, stratified to include v0, v1-format, ALT-heavy, Jupiter and direct-swap transactions.
- Reference outputs: produced offline by the Python/Rust reference for the same transactions, using the recorded
  `resolved_keys` (the dataset's record carries them).
- Compare every column; report mismatches by column and by transaction type. Exact equality is the bar.
