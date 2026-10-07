# framed-tx-v2

`feature_schema = framed-tx-v2`, `schema_id = 2`.

framed-tx-v2 is framed-tx-v1 with two changes:

1. **The v1 transaction format** (`0x81` prefix, Solana `VersionedMessage::V1`) is framed and encoded.
2. **Lookup-loaded accounts use the runtime order** for every legacy/v0 transaction.

Every field, hash constant, data-length class, account-role rule and Compute Budget layout keeps its framed-tx-v1 meaning (see [FRAMED_TX_FEATURES_V1.md](FRAMED_TX_FEATURES_V1.md)). Like v1, this schema is frozen once a model is trained on it.

## Contract

```text
encode_transaction_v2(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV2) -> Status
EncodedTxV2 { base: EncodedTxV1, ext: TxExtV2 }
```

- **On `OK`:**
  - `out.base` has the framed-tx-v1 layout, with `schema_id = 2`.
  - `out.ext` holds the v1-format message config.
  - The canonical populated image is `write_canonical(&out.base)` followed by `out.ext`.
- **On any other status:** `out.base.header` and `out.ext` are zero.
- **Allocation:** none, the same as v1.
- **C ABI:** `txf_encode_v2` / `txf_encoded_v2_bytes`, and the `txf_encoded_v2` struct (`include/tx_features.h`).

`encode_transaction_v1` is unchanged and keeps returning a non-OK status on v1-format bytes.

## First byte

This follows the transaction reader in `solana-transaction` 4.x.

| first byte | framing |
|---|---|
| high bit clear | legacy/v0: compact-u16 signature count, then the message. Same as framed-tx-v1, except for the lookup-table order below |
| `0x81` | v1 format |
| any other byte with the high bit set | `UNSUPPORTED_VERSION` |
| empty input | `MALFORMED` |

## V1 transaction format

Wire layout, from `solana-message` 4.5 (`v1::Message` `SchemaWrite`/`SchemaRead`) and `solana-transaction` 4.2:

```text
0x81
num_required_signatures u8 | num_readonly_signed u8 | num_readonly_unsigned u8
config_mask u32 LE
lifetime_specifier [32]              -- recent blockhash; skipped, not a feature
num_instructions u8 | num_addresses u8
addresses [32] × num_addresses
config values, in this order, each present only if its mask bit is set:
    priority_fee u64 LE              -- mask bits 0 AND 1 (0b11)
    compute_unit_limit u32 LE        -- bit 2
    loaded_accounts_data_size u32 LE -- bit 3
    heap_size u32 LE                 -- bit 4
instruction headers × num_instructions: program_id_index u8 | num_accounts u8 | data_len u16 LE
for each instruction: account indexes [num_accounts] then data [data_len]
signatures [64] × num_required_signatures   -- trailing, no length prefix
```

### Encoding rules

- **Version field.** `version = 1` (`VERSION_V1`).
- **Signatures.** `signature_count = required_signature_count = num_required_signatures`. Signatures are skipped.
- **Accounts.** v1 has **no lookup tables**:
  - every address is a static key (`ORIGIN_STATIC`, with a pubkey identity hash);
  - `alt_lookup_count = 0`, `uses_alt = 0`;
  - `total_account_reference_count = num_addresses`.
- **Writability and signers.** These use the same header arithmetic as legacy/v0, and the fee payer is static index 0.
- **Instructions.** Each instruction's account indexes and data are contiguous byte ranges, so every instruction, account and relational feature is derived by the same code as v1. `program_key_offset`, `data_offset` and `accounts_offset` point into `raw`.
- **Compute Budget.** The header's Compute Budget fields keep their v1 meaning: decoded **only** from top-level ComputeBudget instructions. The v1 message config is reported separately in `ext`.
- **Malformed input.** The following are all `MALFORMED`:
  - unknown mask bits;
  - exactly one of the two priority-fee bits set;
  - header counts that don't fit `num_addresses`;
  - truncation anywhere;
  - out-of-range account or program indexes;
  - any byte after the trailing signatures.

  Sanitization rules (≤12 signatures, ≤64 addresses and instructions, heap alignment, duplicate keys, fee payer as program) are not enforced, the same way v1 doesn't enforce sanitization.

### `TxExtV2`

| field | type | meaning |
|---|---|---|
| `config_priority_fee_lamports` | u64 | v1 priority fee, **in lamports** (not micro-lamports per CU). 0 if absent |
| `config_mask` | u32 | raw `TransactionConfigMask` |
| `config_compute_unit_limit` | u32 | 0 if absent |
| `config_loaded_accounts_data_size` | u32 | 0 if absent |
| `config_heap_size` | u32 | 0 if absent |

Presence is defined by the `config_mask` bits (`V1_CONFIG_*`), not by the value being non-zero. Every `ext` field is zero for legacy and v0.

## Lookup-loaded account order (framed-tx-v1 erratum)

The V1 spec states the runtime order for lookup-loaded rows: static keys, then **every** table's writable indexes in table order, then **every** table's readonly indexes. The framed-tx-v1 implementation, and its test reference, instead expand table by table: `T0.writable, T0.readonly, T1.writable, …`.

- **Where they differ.** For transactions with two or more lookup tables, instruction account indexes that point past the first table's writable block resolve to the wrong loaded row. That row's writable/readonly flag, origin, ALT identity, and everything derived from it are wrong. The derived features are per-instruction writable/ALT counts, writable position patterns and structural families, the instruction-structure and topology fingerprints, and ALT-program identity.
- **Scale.** About 11% of the 100k-transaction corpus and about 65% of p045 transactions use two or more tables.
- **What v1 does.** framed-tx-v1 is frozen, so it keeps this behaviour as a documented erratum.
- **What v2 does.** framed-tx-v2 uses the runtime order. Legacy/v0 transactions with fewer than two tables encode identically under v1 and v2, apart from `schema_id`.

## Validation

- **Independent reference.** `reference::encode_reference_v2` uses a separate byte walk for the v1 format and the runtime lookup order. It matches the fast encoder on every crafted case and on 20k fuzz mutations.
- **Corpus.** All 100,008 transactions of `raw-tx-100k.jsonl`:
  - v2 equals v1 (except `schema_id`) whenever the transaction has fewer than two lookup tables;
  - for the 11,429 with two or more, loaded rows are the same set in runtime order.
- **Real data** (`ml/prep/tests/v1_real_parity.rs`). Every transaction of fleet partition p045 (32,706,063 transactions, 5,292,809 in the v1 format) is checked against the collector's own decoding. That decoding is `wincode::deserialize::<VersionedTransaction>`, round-trip checked. The checked fields are:
  - version and header;
  - every static key's identity hash and signer flag;
  - per-instruction account indexes, data, head bytes and program identity;
  - per-entry signer flags.

  Runtime-writable implies header-writable. 32,884 keys are demoted by the runtime (program ids, reserved accounts); that's expected, because framed-tx writability is header-based by definition.
