# DIRTY-001

First EXACT→DIRTY event per seeded pool. Subsequent GAP counts are
ignored. Do not auto-reanchor during this study.

```text
./build/live_state_001 --cap-list run/cap_list.txt \
    --pools configs/pools/seed.jsonl \
    --luts configs/luts/warm.jsonl \
    --lut-learned run/lut_learned.jsonl \
    --dirty run/first_dirty.jsonl \
    --fam run/pump_fam.jsonl
python3 tools/pump/dirty_report.py run/first_dirty.jsonl run/pump_fam.jsonl
```

Preserve the frozen 8h FIRMCAP1 list. Replay the same files after
implementing a family. AUTH is not this pass.

Framed messages are outer instructions only (`where=outer`).

`DEP_UNKNOWN` is not a final first-DIRTY reason. Lookup splits it at
the raise site:

```text
DEP_MISSING_POOL_ROW
DEP_MISSING_BASE_VAULT_ROW
DEP_MISSING_QUOTE_VAULT_ROW
DEP_MISSING_CONFIG
DEP_MISSING_OTHER_ROW
DEP_EXTRA_IX_TOUCHES_STATE
DEP_EXTRA_IX_NO_STATE_OVERLAP
DEP_UNKNOWN_CPI_EFFECT
DEP_UNKNOWN_PUMP_KIND
```

Each event logs the first offending dependency: signature, slot, pool,
disc, ix_index, program_id, account_pubkey, account_index, writable,
compact_row_present, compact_row_exact.

`DEP_NO_QUOTE_EFFECT` is an apply success, not a first-DIRTY reason.
It is only assigned when:

1. the Solana message header is present (`hdr_ok`) so the writable
   set is trustworthy, and
2. this pool's Pump quote/state dependency set is complete: live pool
   row, non-zero base and quote vaults, `global_config` identity
   known, and protocol/creator fee destinations either mapped or
   present on the outer Pump ix (`nacc` high enough).

Otherwise the extra ix stays fail-closed (`DEP_UNKNOWN_CPI_EFFECT` or
`DEP_MISSING_CONFIG`). ComputeBudget / 0-account ixs cannot write
that set, so they skip completeness.

LUT A/B must freeze the cache first. `cat` warm + learned into one
jsonl, then replay **without** `--lut-learned` (old binary) or with
`--lut-frozen` (new). No miss-list learning. Compare framed / Pump tx /
supported / LUT_MISS before reading EXACT. Dashboard splits
EXACT_UNTOUCHED vs EXACT_ADVANCED vs DIRTY.

`required_by` on first-DIRTY names the CompactState field lookup
failed on (`src_token`, `vault_base`, …). Pump user ATAs and fee
destination balances are not quote-kernel inputs; missing those rows
must not DIRTY an EXACT pool.

Short check: one freeze `.cap`, old `live_state_old` then new
`live_state_001`, same `--luts` file.

Per-signature join (do not relax LUT_STATIC yet):

```text
./build/live_state_old --cap FILE --pools seed.jsonl --luts lut_sealed.jsonl \
    --pred run/ab1/old_pred.jsonl
./build/live_state_001 --cap FILE --pools seed.jsonl --lut-frozen lut_sealed.jsonl \
    --ps-log run/ab1/new_ps.jsonl --dirty run/ab1/new_dirty.jsonl
python3 tools/pump/ps_ab.py run/ab1/old_pred.jsonl run/ab1/new_ps.jsonl
```
