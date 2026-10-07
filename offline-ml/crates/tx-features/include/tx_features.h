#ifndef TX_FEATURES_H
#define TX_FEATURES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* framed-tx-v1 ABI. Field order matches the Rust repr(C) records.
   Only instruction slots [0, header.instruction_count) and account slots
   [0, header.total_account_reference_count) are defined after TXF_OK. */

#define TXF_FEATURE_SCHEMA_ID 1u
#define TXF_MAX_INSTRUCTIONS 768
#define TXF_MAX_ACCOUNTS 256
#define TXF_VERSION_LEGACY 255u
#define TXF_VERSION_V0 0u
/* framed-tx-v2 */
#define TXF_FEATURE_SCHEMA_V2_ID 2u
#define TXF_VERSION_V1 1u
#define TXF_V1_PREFIX 0x81u
#define TXF_V1_CONFIG_PRIORITY_FEE 0x3u
#define TXF_V1_CONFIG_COMPUTE_UNIT_LIMIT 0x4u
#define TXF_V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE 0x8u
#define TXF_V1_CONFIG_HEAP_SIZE 0x10u
#define TXF_INDEX_NONE 65535u

#define TXF_OK 0
#define TXF_MALFORMED 1
#define TXF_UNSUPPORTED_VERSION 2
#define TXF_CAPACITY_EXCEEDED 3
#define TXF_NULL_ARGUMENT 4
#define TXF_INTERNAL 5

#define TXF_PROGRAM_STATIC 1u
#define TXF_PROGRAM_ALT_REF 2u
#define TXF_PROGRAM_UNRESOLVED 3u

#define TXF_ORIGIN_STATIC 1u
#define TXF_ORIGIN_ALT_WRITABLE 2u
#define TXF_ORIGIN_ALT_READONLY 3u

typedef struct txf_header_v1 {
    uint64_t requested_cu_price;
    uint64_t program_sequence_fingerprint;
    uint64_t instruction_structure_fingerprint;
    uint64_t topology_fingerprint;
    uint32_t schema_id;
    uint32_t raw_len;
    uint32_t total_instruction_account_refs;
    uint32_t reused_account_ref_count;
    uint32_t total_instruction_data_bytes;
    uint32_t max_instruction_data_len;
    uint32_t min_instruction_data_len;
    uint32_t compute_budget_flags;
    uint32_t requested_cu_limit;
    uint32_t requested_heap_bytes;
    uint32_t requested_loaded_accounts_data_size;
    uint32_t deprecated_requested_units;
    uint32_t deprecated_additional_fee;
    uint16_t signature_count;
    uint16_t required_signature_count;
    uint16_t readonly_signed_count;
    uint16_t readonly_unsigned_count;
    uint16_t static_account_count;
    uint16_t total_account_reference_count;
    uint16_t static_writable_count;
    uint16_t static_readonly_count;
    uint16_t static_signer_count;
    uint16_t instruction_count;
    uint16_t unique_program_reference_count;
    uint16_t unique_instruction_account_refs;
    uint16_t alt_lookup_count;
    uint16_t alt_loaded_writable_count;
    uint16_t alt_loaded_readonly_count;
    uint16_t compute_budget_instruction_count;
    uint16_t compute_budget_unknown_count;
    uint16_t compute_budget_malformed_count;
    uint16_t compute_budget_first_index;
    uint16_t compute_budget_last_index;
    uint16_t max_account_usage_count;
    uint16_t max_writable_account_usage_count;
    uint16_t instructions_with_shared_accounts;
    uint16_t instructions_with_shared_writable_accounts;
    uint16_t accounts_used_by_multiple_instructions;
    uint16_t writable_accounts_used_by_multiple_instructions;
    uint16_t adjacent_pairs_sharing_account;
    uint16_t adjacent_pairs_sharing_writable;
    uint16_t repeated_program_invocation_count;
    uint8_t version;
    uint8_t uses_alt;
    uint8_t program_repeated;
    uint8_t _pad[7];
} txf_header_v1;

typedef struct txf_instruction_v1 {
    uint64_t program_identity_hash64;
    uint64_t program_alt_identity_hash64;
    uint64_t structural_family_hash64;
    uint64_t writable_position_pattern;
    uint64_t signer_position_pattern;
    uint64_t prefix8;
    uint8_t prefix16[16];
    uint8_t head[32];
    uint32_t data_offset;
    uint32_t data_len;
    uint32_t accounts_offset;
    uint32_t accounts_len;
    uint32_t program_key_offset;
    uint32_t prefix4;
    uint16_t position;
    uint16_t account_count;
    uint16_t signer_account_count;
    uint16_t writable_account_count;
    uint16_t readonly_account_count;
    uint16_t static_account_ref_count;
    uint16_t alt_account_ref_count;
    uint16_t reused_account_count;
    uint16_t reused_writable_account_count;
    uint16_t alt_table_ordinal;
    uint16_t prefix2;
    uint8_t program_ref_kind;
    uint8_t program_account_index;
    uint8_t alt_address_index;
    uint8_t alt_role;
    uint8_t prefix_flags;
    uint8_t prefix1;
    uint8_t head_len;
    uint8_t data_len_class;
    uint8_t position_pattern_complete;
    uint8_t program_identity_available;
} txf_instruction_v1;

typedef struct txf_account_v1 {
    uint64_t identity_hash64;
    uint64_t alt_identity_hash64;
    uint32_t static_pubkey_offset;
    uint32_t alt_table_pubkey_offset;
    uint16_t expanded_index;
    uint16_t static_index;
    uint16_t alt_table_ordinal;
    uint16_t usage_count;
    uint16_t writable_usage_count;
    uint16_t program_usage_count;
    uint16_t first_instruction_seen;
    uint16_t last_instruction_seen;
    uint8_t origin;
    uint8_t signer;
    uint8_t writable;
    uint8_t readonly;
    uint8_t fee_payer;
    uint8_t used_as_program;
    uint8_t used_as_instruction_account;
    uint8_t alt_address_index;
    uint8_t has_static_identity;
    uint8_t static_pubkey_len;
    uint8_t alt_table_pubkey_len;
    uint8_t _pad[5];
} txf_account_v1;

typedef struct txf_encoded_v1 {
    txf_header_v1 header;
    txf_instruction_v1 instructions[TXF_MAX_INSTRUCTIONS];
    txf_account_v1 accounts[TXF_MAX_ACCOUNTS];
} txf_encoded_v1;

/* framed-tx-v2 additions: v1-format message config. Zero for legacy and v0.
   A value is meaningful only when its TXF_V1_CONFIG_* bit is set. */
typedef struct txf_ext_v2 {
    uint64_t config_priority_fee_lamports;
    uint32_t config_mask;
    uint32_t config_compute_unit_limit;
    uint32_t config_loaded_accounts_data_size;
    uint32_t config_heap_size;
} txf_ext_v2;

/* base has the framed-tx-v1 layout (schema_id = 2; version = 1 for the v1 format). */
typedef struct txf_encoded_v2 {
    txf_encoded_v1 base;
    txf_ext_v2 ext;
} txf_encoded_v2;

/* Opaque. Allocate with txf_scratch_bytes() or as txf_scratch.
   txf_encode_v1 zeroes it. */
#define TXF_SCRATCH_BYTES 18432
typedef struct txf_scratch {
    uint64_t opaque[TXF_SCRATCH_BYTES / 8];
} txf_scratch;

size_t txf_scratch_bytes(void);
size_t txf_encoded_v1_bytes(void);
size_t txf_encoded_v2_bytes(void);
uint32_t txf_feature_schema_id(void);
void txf_scratch_init(txf_scratch *scratch);

/* Returns a TXF_* status. Never unwinds into the caller. */
int txf_encode_v1(
    const uint8_t *raw,
    size_t raw_len,
    txf_scratch *scratch,
    txf_encoded_v1 *out);

/* framed-tx-v2. Same contract as txf_encode_v1. */
int txf_encode_v2(
    const uint8_t *raw,
    size_t raw_len,
    txf_scratch *scratch,
    txf_encoded_v2 *out);

#ifdef __cplusplus
}
#endif

#endif
