# framed-tx-v1

`feature_schema = framed-tx-v1`

This schema is frozen once a model is trained on it. A change to any field meaning, hash constant, data-length class boundary, account-role rule, or compute-budget layout is a new schema (`framed-tx-v2`, …). Do not silently revise V1.

The only encoder is `encode_transaction_v1` in `crates/tx-features`. Historical JSONL tooling and the live searcher call that function. There is no Python feature implementation.

## Contract

```text
encode_transaction_v1(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV1) -> Status
```

`raw` is the serialized transaction: compact-u16 signature count, 64-byte signatures, then the legacy or v0 message. It is not JSON, not base58, and not a signature by itself.

On `OK`, the populated image is deterministic:

- `out.header`
- `out.instructions[0..header.instruction_count]`
- `out.accounts[0..header.total_account_reference_count]`

`write_canonical` serializes that prefix. The same raw bytes, historically or live, produce the same canonical bytes. Slots at or above those counts are not part of the contract.

On any other status, `out.header` is zero and the rest of `out` is undefined.

The encoder does not allocate, lock, syscall, verify signatures, hash cryptographically, or read anything except `raw` and the caller-provided buffers. It does not see labels, logs, inner instructions, account state, pool state, or resolved address-lookup contents.

C ABI (`crates/tx-features/include/tx_features.h`):

```c
int txf_encode_v1(const uint8_t *raw, size_t raw_len, txf_scratch *scratch, txf_encoded_v1 *out);
```

A Rust panic is caught at that boundary and returned as `INTERNAL`. The Rust API does not use `catch_unwind`.

## Status

| code | name | meaning |
|---|---|---|
| 0 | `OK` | framed and encoded |
| 1 | `MALFORMED` | truncated, bad shortvec, header counts disagree, index out of range, trailing bytes, signature count ≠ required signatures |
| 2 | `UNSUPPORTED_VERSION` | message version byte is `0x80 \| v` with `v ≠ 0` |
| 3 | `CAPACITY_EXCEEDED` | a count exceeds a V1 table, or `raw.len()` does not fit in `u32` |
| 4 | `NULL_ARGUMENT` | FFI null pointer |
| 5 | `INTERNAL` | FFI panic catch |

Capacity is decided as soon as the count is known, before that payload is required to be present. A count that fits, followed by too few bytes, is `MALFORMED`. Nothing is truncated.

## Capacities

These are protocol bounds, not the pilot maxima. Pilot maxima are in the benchmark report.

| limit | value | why |
|---|---:|---|
| `MAX_ACCOUNTS` | 256 | instruction account indexes and program-id indexes are `u8` |
| `MAX_INSTRUCTIONS` | 768 | a compiled instruction is at least 3 bytes; a 1232-byte packet holds well under 400 empty instructions. 768 covers that packet with margin |
| `MAX_LOOKUP_TABLES` | 64 | each lookup is at least 34 bytes, so 64 already exceeds a 1232-byte packet |

Solana packet payloads are 1232 bytes (`1280 − 40 − 8`). The encoder still accepts a larger buffer until one of the tables fills.

## Groups

Every populated field is knowable from the framed bytes before execution. Groups exist so training can drop columns without a second parse.

| group | use |
|---|---|
| structural | counts, roles, positions, lengths, topology, CU configuration, opcode-width-agnostic prefixes as small integers |
| identity | static pubkey hashes, ALT table/index identity, program-sequence fingerprint |
| raw | spans and the copied instruction head. Metadata for a later sequence model, or byte features |
| meta | schema id and encode status. Not a market feature |
| join | `slot`, `tx_index`, `signature`. Not produced by the encoder. The JSONL adapter adds them. Raw bytes stay in the master JSONL |

Train structural only, structural+identity, or structural+raw from the same `EncodedTxV1`.

Normalization to floats is not done here.

## Wire walk

1. Compact-u16 signature count, then `count × 64` bytes. Skipped. Not features. Must equal `num_required_signatures`.
2. If the next byte has the high bit set, `version = byte & 0x7f` (`0` → v0). Otherwise the message is legacy and that byte is `num_required_signatures`. The version feature is `255` for legacy and `0` for v0.
3. Header: required signatures, readonly signed, readonly unsigned.
4. Compact-u16 static keys, then 32-byte keys.
5. 32-byte recent blockhash. Skipped. Not a feature.
6. Compact-u16 compiled instructions: program index `u8`, compact-u16 account indexes, compact-u16 data.
7. V0 only: compact-u16 address-lookup descriptors. Each is a 32-byte table pubkey, compact-u16 writable address indexes, compact-u16 readonly address indexes.

Expanded account order matches the runtime message: static keys, then every lookup’s writable indexes in table order, then every lookup’s readonly indexes. Duplicate address indexes stay as separate expanded rows. The loaded pubkey is not on the wire and is not invented.

> **Erratum.** The framed-tx-v1 implementation expands lookup-loaded rows table by table (`T0.writable, T0.readonly, T1.writable, …`), not in the runtime order above. The two differ for transactions with two or more lookup tables. framed-tx-v1 is frozen with this behaviour; [framed-tx-v2](FRAMED_TX_FEATURES_V2.md) uses the runtime order and also encodes the v1 transaction format (`0x81`).

Overlong compact-u16 encodings are accepted. More than 3 bytes, a third byte with the continuation bit set, or a value above 65535 is `MALFORMED`.

Header checks: `readonly_signed ≤ required ≤ static_len` and `readonly_unsigned ≤ static_len − required`.

### Static writable bit

For static index `i`:

- if `i < required_signatures`, writable when `i < required_signatures − readonly_signed`
- otherwise writable when `i < static_len − readonly_unsigned`

ALT rows are writable or readonly from which index list they came from. ALT rows are never signers and never the fee payer. `fee_payer` is 1 only for static expanded index 0 (the fee-payer slot), even if the header marks it readonly.

### Program reference

| `program_ref_kind` | value | meaning |
|---|---:|---|
| `STATIC` | 1 | program index selects a static key. `program_identity_hash64` is that key. `program_identity_available = 1` |
| `ALT_REF` | 2 | program index selects a loaded address. The pubkey is not on the wire. `program_identity_hash64 = 0`, `program_identity_available = 0`. `program_alt_identity_hash64` and the ALT ordinal / address index / role describe the wire reference |
| `UNRESOLVED` | 3 | in-range index that is neither static nor a loaded entry. V1 does not emit this on success |

`program_key_offset` is the static pubkey for `STATIC` and the lookup-table pubkey for `ALT_REF`. Both spans are 32 bytes. It is not a resolved account key.

Compute Budget is decoded only when the program is `STATIC` and the 32 bytes equal `ComputeBudget111111111111111111111111111111`. An ALT program is not guessed to be Compute Budget.

## Instruction prefixes and family

Prefixes are little-endian integers of the first N data bytes. A prefix is present only when `data_len ≥ N`. Missing prefixes are 0 and their flag is clear.

| flag bit | prefix |
|---|---|
| 0 | 1 byte |
| 1 | 2 bytes |
| 2 | 4 bytes |
| 3 | 8 bytes |
| 4 | 16 bytes |

`head` is the first `min(32, data_len)` data bytes, zero-padded. This is the only copy of input bytes. `head_len` is that count.

`writable_position_pattern` / `signer_position_pattern` set bit `i` for list position `i` when `i < 64`. `position_pattern_complete = 1` when the account list length is ≤ 64. Positions past 64 are not dropped from the structural family: the family hash mixes every writable position and every signer position, plus the account-list length and the data-length class.

Data-length class:

| class | length |
|---:|---|
| 0 | 0 |
| 1 | 1–2 |
| 2 | 3–4 |
| 3 | 5–8 |
| 4 | 9–16 |
| 5 | 17–32 |
| 6 | 33–64 |
| 7 | 65–128 |
| 8 | 129–256 |
| 9 | 257–512 |
| 10 | 513–1024 |
| 11 | 1025+ |

The exact `data_len` is also stored. CPI structure is not available and is not a feature.

A later trigger registry can form `(program identity or ALT ref, structural family, chosen prefix)` without reparsing.

## Relational definitions

Usage is per instruction, not per repeated index inside one instruction.

- `usage_count`: instructions that reference the account as the program id or in the account-index list
- `writable_usage_count`: instructions that list the account in the account-index array and the account is writable. A program-id-only use does not increment this
- `program_usage_count`: instructions that use the account as the program id
- `reused_account_count` (instruction): distinct accounts listed at least twice in that instruction’s account index list
- `total_instruction_account_refs`: sum of account-index list lengths. Program ids are not included
- `unique_instruction_account_refs`: distinct accounts with `used_as_instruction_account`
- `reused_account_ref_count`: `total_instruction_account_refs − unique_instruction_account_refs`
- `accounts_used_by_multiple_instructions`: accounts with `usage_count > 1`
- `writable_accounts_used_by_multiple_instructions`: accounts with `writable_usage_count > 1`
- `max_account_usage_count`: max `usage_count`. This is “max instructions sharing one account”
- `max_writable_account_usage_count`: max `writable_usage_count`
- `instructions_with_shared_accounts`: instructions whose program id or account list touches an account with `usage_count > 1`
- `instructions_with_shared_writable_accounts`: instructions that list an account with `writable_usage_count > 1`
- `adjacent_pairs_sharing_account`: adjacent instruction pairs whose (program id ∪ account list) sets intersect
- `adjacent_pairs_sharing_writable`: adjacent pairs whose writable account-list sets intersect. Program-id-only use is not a writable use
- `repeated_program_invocation_count`: sum of `program_usage_count − 1` over accounts with `program_usage_count > 1`
- `program_repeated`: 1 when that sum is non-zero
- `unique_program_reference_count`: distinct expanded indexes used as a program id

`first_instruction_seen` / `last_instruction_seen` are instruction indexes. `65535` (`INDEX_NONE`) means the account is on the wire but unused. The same sentinel is used for `static_index` on ALT rows, `alt_table_ordinal` on static rows, and compute-budget first/last indexes when no Compute Budget instruction is present.

When `instruction_count = 0`, min and max instruction data length are 0.

## Compute Budget layouts

Discriminant is one `u8`, then a fixed little-endian payload. A known discriminant with any other length sets `CB_MALFORMED_PAYLOAD` and does not write a numeric field. An unknown discriminant sets `CB_UNKNOWN`. The last well-formed value of each variant wins. A duplicate sets the matching `CB_DUP_*` bit and does not erase the earlier presence bit.

| disc | payload | field | flag bit |
|---:|---|---|---:|
| 0 | `u32` units, `u32` additional fee | `deprecated_requested_units`, `deprecated_additional_fee` | 4, dup 9 |
| 1 | `u32` heap bytes | `requested_heap_bytes` | 2, dup 7 |
| 2 | `u32` CU limit | `requested_cu_limit` | 0, dup 5 |
| 3 | `u64` micro-lamports | `requested_cu_price` | 1, dup 6 |
| 4 | `u32` loaded-accounts data size | `requested_loaded_accounts_data_size` | 3, dup 8 |

Unknown discriminant is bit 10. Malformed payload is bit 11.

Deprecated `RequestUnits` is not copied into `requested_cu_limit`. A present value of 0 is distinguished from absence by the flag. The stored integer is 0 when the flag is clear.

`compute_budget_instruction_count` counts every top-level instruction whose static program id is the Compute Budget program, including unknown and malformed payloads. `compute_budget_first_index` / `compute_budget_last_index` are those instruction positions.

## Hashes

Non-cryptographic. Constants are part of V1.

```text
MIX_CONST        = 0x9E3779B185EBCA87
IDENTITY_IV      = 0x243F6A8885A308D3
IDENTITY_MUL     = 0xBF58476D1CE4E5B9
IDENTITY_MUL2    = 0x94D049BB133111EB
WRITABLE_PATTERN_IV = 0x6A09E667F3BCC909
SIGNER_PATTERN_IV   = 0xBB67AE8584CAA73B
STRUCTURAL_FAMILY_IV = 0x3C6EF372FE94F82B
PROGRAM_SEQUENCE_IV  = 0xA54FF53A5F1D36F1
STRUCTURE_FP_IV      = 0x510E527FADE682D1
TOPOLOGY_FP_IV       = 0x9B05688C2B3E6C1F
```

`mix64(h, v)`: `x = (h + v) * MIX_CONST` (wrapping), then `x ^= x >> 30`.

`identity_hash64`: start at `IDENTITY_IV`. For each little-endian 8-byte lane, `h ^= lane * IDENTITY_MUL`, then `h = rotl(h, 27) * IDENTITY_MUL2`. A short tail is the same with the remaining bytes in the low lanes. Finally `h ^= len`. Frozen vectors: empty → `0x243f6a8885a308d3`, one byte `0x01` → `0xfc697677af4d17e4`, 32 zero bytes → `0x69fd5e9a82e62945`, Compute Budget program id → `0x818e1c968224f269`.

`alt_identity` mixes the lookup-table pubkey hash with ordinal, address index, and origin (`ORIGIN_ALT_WRITABLE = 2`, `ORIGIN_ALT_READONLY = 3`).

`structural_family` mixes `STRUCTURAL_FAMILY_IV` with account-list length, the writable-position running hash, the signer-position running hash, and the data-length class. The position hashes start at their IVs and `mix64` each matching list position. They do not mix a bit for absent roles.

`program_sequence_fingerprint` mixes `PROGRAM_SEQUENCE_IV` with each instruction’s `program_ref_kind`, `program_identity_hash64`, and `program_alt_identity_hash64`, in order.

`instruction_structure_fingerprint` mixes `STRUCTURE_FP_IV` with each `structural_family_hash64`, in order.

`topology_fingerprint` mixes `TOPOLOGY_FP_IV` with instruction count, expanded account count, then each account’s origin, signer, writable, usage_count, writable_usage_count, used_as_program, used_as_instruction_account, then the two adjacent-pair counts. It does not mix pubkey hashes.

## Transaction header fields

In-memory type is the column type. Cost is a fixed write after the linear scan unless noted. “ML” means intended as a model input. “meta” is bookkeeping.

| field | type | group | source | ML |
|---|---|---|---|---|
| `schema_id` | u32 | meta | constant `1` | no |
| `version` | u8 | structural | legacy `255` or v0 `0` | yes |
| `uses_alt` | u8 | structural | lookup count > 0 | yes |
| `raw_len` | u32 | structural | input length | yes |
| `signature_count` | u16 | structural | signature section length. Bytes are not kept | yes |
| `required_signature_count` | u16 | structural | message header | yes |
| `readonly_signed_count` | u16 | structural | message header | yes |
| `readonly_unsigned_count` | u16 | structural | message header | yes |
| `static_account_count` | u16 | structural | static key vector | yes |
| `total_account_reference_count` | u16 | structural | static + loaded index entries | yes |
| `static_writable_count` | u16 | structural | header arithmetic | yes |
| `static_readonly_count` | u16 | structural | header arithmetic | yes |
| `static_signer_count` | u16 | structural | required signatures | yes |
| `instruction_count` | u16 | structural | compiled instruction vector | yes |
| `unique_program_reference_count` | u16 | structural | distinct program indexes | yes |
| `total_instruction_account_refs` | u32 | structural | sum of account-index lists | yes |
| `unique_instruction_account_refs` | u16 | structural | distinct listed accounts | yes |
| `reused_account_ref_count` | u32 | structural | total refs − unique listed accounts | yes |
| `total_instruction_data_bytes` | u32 | structural | sum of data lengths. Mean is this divided by `instruction_count` outside the encoder | yes |
| `max_instruction_data_len` | u32 | structural | max data length | yes |
| `min_instruction_data_len` | u32 | structural | min data length, or 0 if there are no instructions | yes |
| `alt_lookup_count` | u16 | structural | v0 lookup descriptors, including empty ones | yes |
| `alt_loaded_writable_count` | u16 | structural | sum of writable index lists | yes |
| `alt_loaded_readonly_count` | u16 | structural | sum of readonly index lists | yes |
| `compute_budget_instruction_count` | u16 | structural | static Compute Budget program ids | yes |
| `compute_budget_unknown_count` | u16 | structural | unknown discriminants, including empty data | yes |
| `compute_budget_malformed_count` | u16 | structural | known discriminant, wrong length | yes |
| `compute_budget_first_index` | u16 | structural | first such instruction, or `65535` | yes |
| `compute_budget_last_index` | u16 | structural | last such instruction, or `65535` | yes |
| `compute_budget_flags` | u32 | structural | bits documented above | yes |
| `requested_cu_limit` | u32 | structural | last well-formed disc 2. 0 if absent | yes |
| `requested_cu_price` | u64 | structural | last well-formed disc 3. 0 if absent | yes |
| `requested_heap_bytes` | u32 | structural | last well-formed disc 1 | yes |
| `requested_loaded_accounts_data_size` | u32 | structural | last well-formed disc 4 | yes |
| `deprecated_requested_units` | u32 | structural | last well-formed disc 0 | yes |
| `deprecated_additional_fee` | u32 | structural | last well-formed disc 0 | yes |
| `max_account_usage_count` | u16 | structural | max instructions sharing one account | yes |
| `max_writable_account_usage_count` | u16 | structural | max writable-list sharing | yes |
| `instructions_with_shared_accounts` | u16 | structural | see relational definitions | yes |
| `instructions_with_shared_writable_accounts` | u16 | structural | see relational definitions | yes |
| `accounts_used_by_multiple_instructions` | u16 | structural | see relational definitions | yes |
| `writable_accounts_used_by_multiple_instructions` | u16 | structural | see relational definitions | yes |
| `adjacent_pairs_sharing_account` | u16 | structural | see relational definitions | yes |
| `adjacent_pairs_sharing_writable` | u16 | structural | see relational definitions | yes |
| `program_repeated` | u8 | structural | any program index used more than once | yes |
| `repeated_program_invocation_count` | u16 | structural | extra invocations | yes |
| `program_sequence_fingerprint` | u64 | identity | program kinds and identity hashes, in order | yes, identity models |
| `instruction_structure_fingerprint` | u64 | structural | structural families, in order | yes |
| `topology_fingerprint` | u64 | structural | account roles and usage, no pubkeys | yes |

Cost of the header is one linear pass over instruction account-index bytes, a second pass for shared-instruction counts, and a pass over the expanded account table. No maps.

## Instruction fields

One `InstructionFeaturesV1` per top-level compiled instruction. About 152 bytes. Filling one record is a scan of that instruction’s account indexes plus a copy of at most 32 data bytes.

| field | type | group | source | ML |
|---|---|---|---|---|
| `position` | u16 | structural | instruction index | yes |
| `program_ref_kind` | u8 | structural | static / ALT ref / unresolved | yes |
| `program_account_index` | u8 | structural | compiled program id index | yes |
| `program_identity_available` | u8 | identity | 1 only for a static program pubkey | yes, as a mask |
| `program_identity_hash64` | u64 | identity | static pubkey hash, else 0 | identity models |
| `program_alt_identity_hash64` | u64 | identity | ALT descriptor hash, else 0 | identity models |
| `program_key_offset` | u32 | raw | byte offset of the static key or lookup-table key | metadata |
| `alt_table_ordinal` | u16 | identity | lookup ordinal, or `65535` | identity models |
| `alt_address_index` | u8 | identity | index inside the lookup table | identity models |
| `alt_role` | u8 | structural | 0 none, 1 writable, 2 readonly | yes |
| `account_count` | u16 | structural | account-index list length | yes |
| `signer_account_count` | u16 | structural | list entries that are signers | yes |
| `writable_account_count` | u16 | structural | list entries that are writable | yes |
| `readonly_account_count` | u16 | structural | list entries that are readonly | yes |
| `static_account_ref_count` | u16 | structural | list entries with index < static len | yes |
| `alt_account_ref_count` | u16 | structural | the rest of the list | yes |
| `reused_account_count` | u16 | structural | distinct accounts repeated inside this list | yes |
| `reused_writable_account_count` | u16 | structural | those that are writable | yes |
| `data_len` | u32 | structural | instruction data length | yes |
| `data_len_class` | u8 | structural | class table | yes |
| `data_offset` | u32 | raw | offset of data bytes in `raw` | metadata |
| `accounts_offset` | u32 | raw | offset of account-index bytes | metadata |
| `accounts_len` | u32 | raw | same as `account_count` | metadata |
| `prefix_flags` | u8 | raw | which prefixes exist | yes |
| `prefix1` | u8 | raw | first byte as an integer | yes |
| `prefix2` | u16 | raw | first 2 bytes, LE | yes |
| `prefix4` | u32 | raw | first 4 bytes, LE | yes |
| `prefix8` | u64 | raw | first 8 bytes, LE | yes |
| `prefix16` | 16 bytes | raw | first 16 bytes | yes, as bytes |
| `head_len` | u8 | raw | `min(32, data_len)` | yes |
| `head` | 32 bytes | raw | first 32 data bytes, zero padded | yes, as bytes |
| `writable_position_pattern` | u64 | structural | first 64 list positions | yes |
| `signer_position_pattern` | u64 | structural | first 64 list positions | yes |
| `position_pattern_complete` | u8 | structural | list length ≤ 64 | yes |
| `structural_family_hash64` | u64 | structural | family mix | yes |

`ix_head` in Parquet is the 32-byte head. Individual head bytes are the GBDT byte features; the span is what a sequence model tokenizes. The full data bytes are not copied into the feature file. `data_offset` + `data_len` address the master raw transaction.

## Account fields

One `AccountRoleV1` per expanded account reference, static keys first, then loaded writable indexes, then loaded readonly indexes.

| field | type | group | source | ML |
|---|---|---|---|---|
| `expanded_index` | u16 | structural | position in the expanded list | yes |
| `origin` | u8 | structural | 1 static, 2 ALT writable, 3 ALT readonly | yes |
| `static_index` | u16 | structural | index among static keys, or `65535` | yes |
| `signer` | u8 | structural | static index < required signatures | yes |
| `writable` | u8 | structural | header or ALT list | yes |
| `readonly` | u8 | structural | complement of writable | yes |
| `fee_payer` | u8 | structural | static index 0 | yes |
| `usage_count` | u16 | structural | distinct instructions that reference it | yes |
| `writable_usage_count` | u16 | structural | instructions that list it as writable | yes |
| `program_usage_count` | u16 | structural | instructions that use it as the program | yes |
| `first_instruction_seen` | u16 | structural | or `65535` | yes |
| `last_instruction_seen` | u16 | structural | or `65535` | yes |
| `used_as_program` | u8 | structural | | yes |
| `used_as_instruction_account` | u8 | structural | | yes |
| `identity_hash64` | u64 | identity | static pubkey hash, else 0 | identity models |
| `has_static_identity` | u8 | identity | 1 when the hash is a pubkey hash | yes, as a mask |
| `static_pubkey_offset` | u32 | raw | offset of the 32-byte static key, meaningful when `static_pubkey_len = 32` | metadata |
| `static_pubkey_len` | u8 | raw | 32 or 0 | metadata |
| `alt_identity_hash64` | u64 | identity | table/index/role hash, else 0 | identity models |
| `alt_table_ordinal` | u16 | identity | or `65535` | identity models |
| `alt_address_index` | u8 | identity | address index byte | identity models |
| `alt_table_pubkey_offset` | u32 | raw | lookup-table pubkey, not the loaded account | metadata |
| `alt_table_pubkey_len` | u8 | raw | 32 or 0 | metadata |

## What V1 deliberately does not emit

Signature bytes, the recent blockhash, resolved ALT account pubkeys, token decimals, balances, pool reserves, inner instructions, logs, success, compute units consumed, and any label. No base58. No float. No entropy. No dynamic map on the hot path.

## Offline path

`tx-features encode-jsonl --input raw.jsonl --output features.parquet` hex-decodes `raw_hex` and calls `encode_transaction_v1`. It writes join columns `slot`, `tx_index`, `signature`, plus `feature_schema = framed-tx-v1` and the encode status. It does not write `raw_hex`. Nested instruction and account integers are stored as uint64 lists with `source_width` metadata equal to the in-memory width. Values are the same integers, zero-extended.

`tx-features join-labels` is a generic left join on a key, default `signature`. Label columns are prefixed `label_` and are not interpreted. The encoder crate does not depend on that command and has no label types.

```text
features.parquet  JOIN  economic_labels  ON signature
```

## Hot path

```text
framed bytes
  → encode_transaction_v1
  → EncodedTxV1
  → trigger family from (program ref, structural family, selected prefix)
  → model
```

Reuse one `Scratch` and one `EncodedTxV1`. `EncodedTxV1` is about 128 KiB; `EncodedTxV1::boxed` allocates it off the stack once. `Scratch` is 18432 bytes. The C entry additionally zeroes scratch because C memory may be uninitialized. The Rust entry only clears the stamp prefix for the accounts this transaction uses.
