# Landing head: P(landed_ok)

Bundle `landing-v1`, schema `firm-landing-bundle-v1`. A second model that runs next to `pool-ev-v1`. For a
transaction seen on the feed, it predicts whether that transaction will **execute successfully**, using only what
is known before it executes.

## Why it exists

Your M3 shadow run found that live decisions landed only 19% of the time, against 85% for the offline sample.
The cause is in the training data, not the live path. `pool-ev-v1` was trained only on transactions that
**committed and wrote a venue account**, so every training example had already succeeded. Live, the trader sees
every transaction, including the many that will fail (bots spamming the same arb, slippage and stale-quote
failures). The arb model has never seen a failed transaction, so it cannot tell them apart. The leakage audit
missed this: see [09-leakage-audit.md](09-leakage-audit.md#missed-selection-on-outcome).

This head covers the missing factor:

```
EV(tx) = P(land | tx) × P(arb | tx, land) × E[size | arb]
         ^ landing-v1   ^ pool-ev-v1 (approximately, see below)
```

## What it predicts

| | |
|---|---|
| Target | `landed_ok` = the transaction executed without error (runtime `success`). |
| Population | Non-vote transactions in finalized blocks whose **ALT-resolved** keys include at least one pool in `pools.parquet` (20,762 pools of Raydium CLMM, Orca Whirlpool, Meteora DLMM: the labelled venues). Landed and failed alike. |
| Label source | The collector's `runtime.parquet` (`metadata.success`). It is used for the label and for the reputation counts, never as a feature of the transaction itself. |
| Inputs | 35 static inputs decoded from the raw transaction, plus 5 fee-payer reputation inputs (all causal): 40 in total. No account state, no simulation. |
| Data | 12 partitions from 30 Sep to 2 Oct 2026, 54.9M transactions. Split by slot: train 37.6M (slots 452160884–452563654); val 6.4M (to 452649906, 61.6% landed; early stopping and cut-offs); test 10.8M (to 452736205, 52.7% landed; reported once). |

**"Landed" here means "succeeded given that it is in a block".** On the shred feed, every transaction you see
is already in a leader's block, so success or failure is the only open question, and that is exactly what this
head predicts. Transactions that were never included (dropped or expired) don't appear on the feed and are not
part of the problem. The exception is forked or skipped blocks; these are rare, and both sides ignore them.

## Fetch

```bash
cd offline-ml/ml
uv run --with boto3 python weekly/fetch.py --out ./data --credentials tigris.json --bundle landing-v1
```

Tigris prefix: `ml/labeller-week-20260925-001/run-001/bundles/landing-v1`. It uses the same publication
convention as `pool-ev-v1` (`publication.json` with a sha256 for every file, then `COMPLETE`), and the fetch
script checks every file. On the Frankfurt box the bundle is already at
`/data/bsc/captures/firm-ml/bundles/landing-v1`.

| File | Contents |
|---|---|
| `manifest.json` | Schema, `feature_order`, feature kinds, hash function, reputation parameters, population rule, slot windows, metrics, file list |
| `landing_model.txt` | LightGBM text model (score with `num_iteration = best_iteration`, which is stored in `landing_model.json`) |
| `landing_model.json` | Model metadata: parameters, best iteration, column order after encoding |
| `landing_model.encoder.json` | Hash vocabularies (`vocab[column][str(hash)] -> code`), fit on train |
| `pools.parquet` | The pool universe: `pool`, `program`, `token_a`, `token_b` |
| `constants.json` | Venue program ids, Jupiter v6, WSOL mint, the 8 Jito tip accounts, System and ComputeBudget ids, venue swap discriminators (hex) |
| `eval.json` | Val and test metrics, gate trade-off tables (val and test), calibration, subsets, feature importance |
| `parity_model.parquet` | 50,000 test rows: `signature`, `slot`, `fee_payer`, every raw model input, and the expected `p_land` |
| `parity_features.parquet` | Every population transaction in slots 450835794–450836600, the window of `~/fixtures/orbitflare-20260927-002844.cap`: `signature`, position, `fee_payer`, `landed_ok`, and the 35 static inputs. Use it to check a feature builder against the frozen capture. |

The reference implementation is in this repo:

- `offline-ml/ml/experiments/extract_population.py` (`features()`) builds the static inputs.
- `offline-ml/ml/experiments/landing_head.py` (`reputation()`) builds the reputation inputs.
- `offline-ml/ml/experiments/export_landing.py` trains the model and writes this bundle.

## Feature contract

The input order is `manifest.json["feature_order"]`. Encoding follows
[03-feature-contract.md](03-feature-contract.md): `hash` inputs go through the vocabulary (unseen or rare →
missing), and `numeric` inputs are the integer or float value as float64. Missing values must reach the trees as
LightGBM missing (NaN), not as 0.

Definitions:

- `keys` = static keys followed by ALT-resolved keys (writable lookups, then read-only, the standard v0 order).
- `ixs` = top-level instructions only; inner instructions are not visible before execution.
- `u64(b, at)` = 8 bytes little-endian.

### Program identity (hash)

`h64(text) = blake2b(utf8(text), digest_size=8)`, read little-endian as u64, then `>> 1`. The text is the base58
program id.

| Input | Definition |
|---|---|
| `prog0`, `prog1`, `prog2` | `h64` of the program id of top-level instruction 0, 1, 2; `0` if that instruction does not exist |
| `prog_last` | `h64` of the last instruction's program id; `0` if there are no instructions |
| `prog_seq` | `h64("|".join(program ids of all top-level instructions, excluding ComputeBudget))` |

These are cheap to precompute: one table from program id to `h64`, plus a running hash for `prog_seq`. The
`prog_seq` value depends on the exact string join, so build it from the base58 strings or precompute it per
observed sequence.

### Transaction shape (numeric)

| Input | Definition |
|---|---|
| `version` | 0 for legacy, 1 for v0 |
| `uses_alt` | 1 if `len(keys) > len(static keys)` |
| `n_alt_tables` | number of address-table lookups in the message |
| `n_static_keys`, `n_keys` | counts of static keys, of all keys after ALT resolution |
| `n_signers` | `header.num_required_signatures` |
| `n_writable` | number of writable keys over all keys (message-level writability, including writable ALT entries) |
| `n_instructions` | top-level instruction count |
| `n_distinct_programs` | distinct top-level program ids |
| `data_bytes`, `max_ix_data` | sum and maximum of top-level instruction data lengths |
| `raw_len` | serialized transaction length in bytes (signatures included) |
| `n_compute_budget_ix` | number of ComputeBudget instructions |

### Fees and tips (numeric)

| Input | Definition |
|---|---|
| `cu_limit` | requested compute-unit limit (ComputeBudget `SetComputeUnitLimit`), 0 if absent |
| `cu_price` | ComputeBudget `SetComputeUnitPrice` in micro-lamports, 0 if absent |
| `priority_fee` | `cu_limit * cu_price // 1_000_000` (integer division) |
| `jito_tip` | sum of lamports of top-level System transfers (`u32 LE data[0:4] == 2`, data ≥ 12 bytes, lamports `u64(data, 4)`) whose destination `keys[ix.accounts[1]]` is a Jito tip account |
| `has_tip` | `jito_tip > 0` |
| `n_transfers` | number of top-level System transfers (any destination) |
| `durable_nonce` | 1 if the transaction uses a durable nonce (first instruction is System `AdvanceNonceAccount`) |
| `payer_only_signer` | `n_signers == 1` |

When no `SetComputeUnitLimit` is present, `cu_limit` is 0 rather than the runtime default. Keep it that way.

### Pools and swaps (numeric)

A pool is "named" if it is in `pools.parquet`.

| Input | Definition |
|---|---|
| `n_pools` | distinct named pools among `keys` |
| `n_pools_static` | named pools found among the static keys (counted per key occurrence) |
| `n_pools_writable` | named pools in a writable position (counted per key occurrence) |
| `n_venue_swaps` | top-level instructions whose program is a venue, data ≥ 16 bytes, and `data[0:8]` is one of the swap discriminators in `constants.json` |
| `max_venue_swap` | the maximum of `u64(data, 8)` over those instructions (the amount argument) |
| `n_jup_routes` | top-level Jupiter v6 instructions with data ≥ 27 bytes |
| `max_jup_in` | the maximum of `u64(data, len(data) - 19)` over those (the `in_amount` of route-style instructions) |
| `wsol_in_keys` | WSOL mint among `keys` |
| `attempted_arb_static` | the named pools' (`token_a`, `token_b`) pairs contain a cycle through WSOL (needs 2+ pools; see `wsol_cycle` in `extract_population.py`) |

The swap and Jupiter counts look only at top-level instructions. Swaps that a router reaches through CPI are not
visible before execution. The pool counts still see them, because the pools are in the keys.

### Fee-payer reputation (numeric, causal)

The model leans heavily on these, and they are the inputs most likely to differ between offline and live, so
match them carefully.

| Parameter | Value |
|---|---|
| Key | fee payer = static key 0 |
| Events | the outcome (landed or failed) of **every population transaction**, i.e. any transaction naming a pool in `pools.parquet`, not just the ones you score |
| Visibility | an event at slot `t` counts for a transaction at slot `s` only if `t ≤ s − 3` |
| Decay | per payer, both counts are multiplied by `0.5^(e − e_last)` where `e = floor(slot / 9000)` (about one hour per epoch) |

Inputs: `rep_landed`, `rep_failed` (decayed counts as floats), `rep_total` = their sum, `rep_fail_rate` =
`rep_failed / rep_total`, **NaN when `rep_total == 0`**, and `rep_seen` = 1 if the payer has any visible event.
A payer never seen gives `0, 0, 0, NaN, 0`.

The decay is stepwise by epoch boundary, not continuous. When an event is applied, the payer's counts are first
decayed to the event's epoch. When a transaction is scored at slot `s`, the stored counts are decayed to
`floor(s / 9000)`. The exact loop is `reputation()` in `landing_head.py` (about 25 lines). Port it as written.

Building it live:

- **Source of truth.** Your truth recorder already tracks payer outcomes for the policy B reputation gate.
  Feed it the outcome of every transaction naming a pool in `pools.parquet`, not only the arb candidates. Use
  `confirmed` commitment or better; offline used finalized outcomes.
- **Lag.** The model was trained with a 2-slot gap (outcomes visible from `s − 3`). If your truth arrives later
  than that, the live counts are staler than training. Measure the real arrival lag (slot of the transaction to
  slot at which its outcome is applied) and report it. If p50 is above 2 slots, tell the owner and we will
  retrain with the measured lag.
- **Warm-up.** Counts start at zero when the process starts. Start scoring with the gate only after one full
  epoch (9,000 slots, about 1 hour) of warm-up, or load a snapshot of the reputation table on start.
- **Memory.** Most payers are rare. An open-addressing table of `payer → (landed f32, failed f32, epoch u32)`,
  pruned of entries whose decayed total is below 0.01, stays small.

## How to use it

### Phase 1: a gate (policy C), logged next to policy B

Keep the arb decision exactly as it is. Then:

```
decision_B = arb_decision and not bad_payer           # your current reputation gate
decision_C = arb_decision and p_land >= CUTOFF         # this head
```

Log `p_land`, the five reputation inputs, and both decisions for every arb decision and for the deterministic
sample of non-decisions (add the fields to the [05-paper-trading-spec.md](05-paper-trading-spec.md) schema).
Score both policies in M4 against the realised outcome.

Cut-offs fitted on val (from `eval.json` `gate_val`; test shown for honesty):

| Cut-off | Val: kept | Val: landed rate of kept | Val: failures removed | Val: landed lost | Test: landed rate of kept | Test: failures removed | Test: landed lost |
|---|---|---|---|---|---|---|---|
| 0.05 | 71.7% | 85.6% | 73.1% | 0.4% | 84.5% | 79.7% | 0.4% |
| 0.1 | 67.5% | 90.5% | 83.3% | 0.9% | 90.8% | 88.8% | 0.8% |
| **0.2** | 64.1% | 94.6% | 91.0% | 1.6% | 94.6% | 93.7% | 1.4% |
| 0.3 | 62.4% | 96.5% | 94.3% | 2.3% | 96.2% | 95.7% | 1.9% |
| **0.5** | 60.0% | 98.7% | 98.0% | 3.9% | 98.3% | 98.2% | 3.4% |
| 0.7 | 59.4% | 99.1% | 98.6% | 4.4% | 98.9% | 98.8% | 3.9% |
| 0.9 | 57.9% | 99.6% | 99.4% | 6.3% | 99.5% | 99.4% | 5.9% |
| *Policy B: bad payer (≥ 20 prior, ≥ 90% failed), test* | 67.7% | | | | 77% | 67% | 1.1% |

Base landing rate before any gate: val 61.6%, test 52.7%. Policy B uses the same decayed counts as the model's
reputation inputs (`rep_total ≥ 20 and rep_fail_rate ≥ 0.9`), so this row is a like-for-like reference, not your
exact live implementation.

- **Start with 0.5.** It removes almost all failures and loses few landed transactions.
- **Use 0.2** if the arb budget is tight and losing landed arbs costs more than chasing failures.

Never refit the cut-off on live data without telling the owner.

### Phase 2: the expected value product

Once phase 1 shows that the live landing rate of gated decisions matches offline, rank by

```
EV = p_land × P(arb) × expected_size
```

rather than gating. `pool-ev-v1`'s score is an approximation of `P(arb | land)`, because it was trained on
landed transactions only. That is the term we want here, so the product is consistent. Keep the gate
from phase 1 as a floor (`p_land ≥ 0.05`), and refit the budget threshold on the product over the same val
window. A labeller rerun is coming that will provide `P(arb)` trained on the full population, including failed
transactions (see below). At that point this product is replaced by the new bundle's own EV.

### Latency

The 35 static inputs take one pass over the instructions and keys you already walk for `pool-ev-v1`'s features.
Reputation is one hash probe. The full model, however, is about 3,000 trees: fine for parity and offline
scoring, too slow for the 1 µs budget. Do the same as M2:

1. Port the full model and prove parity (L2).
2. Distil or retrain a compact variant (for example ≤ 200 trees, ≤ 31 leaves) on the same train/val split.
3. Accept it if, at the val cut-off that keeps the same `landed_lost`, it removes at least 97% of the failures
   the full model removes.
4. Ship the compact model. Report feature build plus score p99 separately (target ≤ 1 µs on top of the arb path).

If the compact model misses that bar, a two-stage design is acceptable:
- score the arb model first;
- run the full landing model only on the transactions that pass the arb threshold (about 1,000 per hour).

At that rate tens of µs per score is irrelevant to throughput. It only adds latency to the decisions that are
sent, so report that added latency.

## Results

From `eval.json`: test window, scored once.

| | Rows | Landed | ROC-AUC | Failure PR-AUC | Brier |
|---|---|---|---|---|---|
| Val | 6.44M | 61.6% | 0.995 | 0.991 | 0.024 |
| **Test** | 10.82M | 52.7% | **0.996** | **0.995** | 0.020 |
| Test: `attempted_arb_static = 1` | 3.53M | 74.3% | 0.999 | 0.997 | 0.009 |
| Test: Jupiter route | 1.07M | 31.8% | 0.977 | 0.987 | 0.041 |
| Test: direct venue swap instruction | 0.37M | 43.1% | 0.938 | 0.942 | 0.090 |
| Test: bad payers (policy B flags) | 3.50M | 1.9% | 0.901 | 0.998 | 0.013 |

From the same test window, ablations of an earlier run that kept the two block-position inputs: reputation only
ROC 0.976; static inputs only 0.984; everything 0.996.

Calibration on test (predicted bin → realised landing rate). Most mass sits at the two ends. The thin middle
runs a little hot around 0.5–0.7, so treat `p_land` as a ranking there, not as a probability.

| Bin | 0–0.1 | 0.1–0.2 | 0.2–0.3 | 0.3–0.4 | 0.4–0.5 | 0.5–0.6 | 0.6–0.7 | 0.7–0.8 | 0.8–0.9 | 0.9–1 |
|---|---|---|---|---|---|---|---|---|---|---|
| Rows | 4.59M | 281k | 133k | 105k | 105k | 33k | 28k | 42k | 102k | 5.40M |
| Landed | 1.0% | 10.6% | 24.5% | 34.5% | 45.1% | 41.2% | 51.5% | 67.4% | 82.3% | 99.5% |

Top inputs by gain:
- `rep_fail_rate`
- `prog_seq`
- `jito_tip`
- `rep_failed`
- `rep_landed`
- `has_tip`
- `prog2`, `prog_last`, `prog0`
- `rep_total`
- `prog1`
- `durable_nonce`

The model mostly learns who is sending and what bot program sequence they run.

`parity_features.parquet` has 91,371 rows.

## Acceptance

**L1: features.**
- Your builder against `parity_features.parquet` must match exactly on all 35 static inputs (hash inputs before
  vocabulary lookup). Build the transactions from the frozen capture `orbitflare-20260927-002844.cap` through
  the normal TXFRAME → ALT path.
- Rows the capture lacks (missed shreds) or cannot resolve (ALT miss) are counted and reported, not dropped
  silently.

**L2: model.**
- Feed `parity_model.parquet`'s raw inputs (columns in `feature_order`) through your encoder and scorer. The
  result must match `p_land` to within 1e-9 on all 50,000 rows, including the rows with NaN `rep_fail_rate`.

**L3: reputation.**
- Run your live reputation builder offline over a recorded day of outcomes. Compare it with `reputation()` in
  Python on the same events: exact match on all five inputs.
- Then report the live truth-arrival lag (see above).

**L4: live.**
- Run policy C next to B for at least 24 hours of shadow trading. Report:
  - the share of decisions each policy keeps;
  - the realised landing rate of kept decisions (target: within 5 points of offline `landed_rate_kept` at the
    same cut-off);
  - landed arbs that C removed and B kept, and the reverse, with examples;
  - the `p_land` distribution live against offline (deciles; flag any shift above 10%).

## Caveats

1. **Short window.** Trained and tested on 3 days (30 Sep to 2 Oct). Live drift is unknown. The L4 run is the
   real test, and 8 Oct logs can be scored offline first.
2. **Python features, not `txflat-v2`.** This head uses its own small feature set, not the 173 inputs of
   `pool-ev-v1`. That makes it easy to port, but it is a second feature builder to maintain.
3. **Reputation dominates, and the hardest slice is the one that looks most like us.** Reputation alone scores
   ROC 0.976 and static inputs alone 0.984; together they score 0.996. Transactions with a direct venue swap
   instruction are the hard slice (ROC 0.938, Brier 0.090). A bot whose payer rotates every transaction leans
   on the static inputs only. The test numbers include such payers, but watch the `rep_seen == 0` slice live.
4. **Population approximation.** "Names a pool in `pools.parquet`" approximates the labeller's population.
   Writability is message-level (from ALT resolution), which is what you have live.
5. **Not a profit model.** A transaction that lands is not necessarily a profitable arb. The landing head says
   nothing about `P(arb)`.

## What changes next

The labeller is being rerun with over 90% coverage, and it will publish the labels this head needs natively:
- failed and read-only transactions kept in the population;
- `landed_ok` and error kind;
- realised profit;
- static `attempted_arb`.

When that lands:

1. `landing-v2` will be retrained on the labeller's population, which replaces the approximation in caveat 4,
   with the same feature contract, unless a change is announced in its manifest.
2. A new arb bundle trained on the full population will be published, with `P(arb)` and the combined EV
   computed in one place.

Your code should load any bundle by path and check `manifest.json["schema"]`. Code built to this document then
carries over unchanged.
