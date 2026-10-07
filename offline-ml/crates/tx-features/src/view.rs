//! In-memory `framed-tx-v1` records.
//!
//! Layout is `repr(C)` and is the C ABI. Equality of two encodings is the
//! populated prefix: the header, the first `instruction_count` instruction
//! records, and the first `total_account_reference_count` account records.
//! Slots at or above those counts are not part of the contract.

use crate::limits::{MAX_ACCOUNTS, MAX_INSTRUCTIONS};

/// Transaction-level features. Every integer is the raw value; there are no floats.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TxHeaderV1 {
    pub requested_cu_price: u64,
    pub program_sequence_fingerprint: u64,
    pub instruction_structure_fingerprint: u64,
    pub topology_fingerprint: u64,
    pub schema_id: u32,
    pub raw_len: u32,
    pub total_instruction_account_refs: u32,
    pub reused_account_ref_count: u32,
    pub total_instruction_data_bytes: u32,
    pub max_instruction_data_len: u32,
    pub min_instruction_data_len: u32,
    pub compute_budget_flags: u32,
    pub requested_cu_limit: u32,
    pub requested_heap_bytes: u32,
    pub requested_loaded_accounts_data_size: u32,
    pub deprecated_requested_units: u32,
    pub deprecated_additional_fee: u32,
    pub signature_count: u16,
    pub required_signature_count: u16,
    pub readonly_signed_count: u16,
    pub readonly_unsigned_count: u16,
    pub static_account_count: u16,
    pub total_account_reference_count: u16,
    pub static_writable_count: u16,
    pub static_readonly_count: u16,
    pub static_signer_count: u16,
    pub instruction_count: u16,
    pub unique_program_reference_count: u16,
    pub unique_instruction_account_refs: u16,
    pub alt_lookup_count: u16,
    pub alt_loaded_writable_count: u16,
    pub alt_loaded_readonly_count: u16,
    pub compute_budget_instruction_count: u16,
    pub compute_budget_unknown_count: u16,
    pub compute_budget_malformed_count: u16,
    pub compute_budget_first_index: u16,
    pub compute_budget_last_index: u16,
    pub max_account_usage_count: u16,
    pub max_writable_account_usage_count: u16,
    pub instructions_with_shared_accounts: u16,
    pub instructions_with_shared_writable_accounts: u16,
    pub accounts_used_by_multiple_instructions: u16,
    pub writable_accounts_used_by_multiple_instructions: u16,
    pub adjacent_pairs_sharing_account: u16,
    pub adjacent_pairs_sharing_writable: u16,
    pub repeated_program_invocation_count: u16,
    /// [`crate::VERSION_LEGACY`] or [`crate::VERSION_V0`].
    pub version: u8,
    pub uses_alt: u8,
    pub program_repeated: u8,
    pub _pad: [u8; 7],
}

/// One top-level compiled instruction.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InstructionFeaturesV1 {
    pub program_identity_hash64: u64,
    pub program_alt_identity_hash64: u64,
    pub structural_family_hash64: u64,
    /// Bit `i` is set when list position `i` (< 64) is writable.
    pub writable_position_pattern: u64,
    /// Bit `i` is set when list position `i` (< 64) is a signer.
    pub signer_position_pattern: u64,
    pub prefix8: u64,
    pub prefix16: [u8; 16],
    pub head: [u8; 32],
    pub data_offset: u32,
    pub data_len: u32,
    pub accounts_offset: u32,
    pub accounts_len: u32,
    /// Static pubkey, or the lookup-table pubkey when the program is an ALT ref.
    pub program_key_offset: u32,
    pub prefix4: u32,
    pub position: u16,
    pub account_count: u16,
    pub signer_account_count: u16,
    pub writable_account_count: u16,
    pub readonly_account_count: u16,
    pub static_account_ref_count: u16,
    pub alt_account_ref_count: u16,
    pub reused_account_count: u16,
    pub reused_writable_account_count: u16,
    /// [`crate::INDEX_NONE`] when the program is not an ALT reference.
    pub alt_table_ordinal: u16,
    pub prefix2: u16,
    pub program_ref_kind: u8,
    pub program_account_index: u8,
    pub alt_address_index: u8,
    pub alt_role: u8,
    pub prefix_flags: u8,
    pub prefix1: u8,
    pub head_len: u8,
    pub data_len_class: u8,
    /// 1 when `account_count <= 64` and the u64 patterns cover every position.
    pub position_pattern_complete: u8,
    /// 1 only when `program_identity_hash64` is the static pubkey hash.
    pub program_identity_available: u8,
}

/// One static key or one loaded ALT address index, in expanded-index order.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AccountRoleV1 {
    /// Static pubkey hash. Zero when the account is not static.
    pub identity_hash64: u64,
    /// Lookup-table/index/role hash. Zero when the account is static.
    pub alt_identity_hash64: u64,
    pub static_pubkey_offset: u32,
    pub alt_table_pubkey_offset: u32,
    pub expanded_index: u16,
    /// [`crate::INDEX_NONE`] for ALT-loaded entries.
    pub static_index: u16,
    /// [`crate::INDEX_NONE`] for static keys.
    pub alt_table_ordinal: u16,
    pub usage_count: u16,
    pub writable_usage_count: u16,
    pub program_usage_count: u16,
    pub first_instruction_seen: u16,
    pub last_instruction_seen: u16,
    pub origin: u8,
    pub signer: u8,
    pub writable: u8,
    pub readonly: u8,
    pub fee_payer: u8,
    pub used_as_program: u8,
    pub used_as_instruction_account: u8,
    pub alt_address_index: u8,
    pub has_static_identity: u8,
    /// 32 or 0.
    pub static_pubkey_len: u8,
    /// 32 or 0. This span is the lookup table pubkey, not the loaded account.
    pub alt_table_pubkey_len: u8,
    pub _pad: [u8; 5],
}

/// Full V1 encoding. Caller-owned. The encoder does not allocate it.
#[repr(C)]
pub struct EncodedTxV1 {
    pub header: TxHeaderV1,
    pub instructions: [InstructionFeaturesV1; MAX_INSTRUCTIONS],
    pub accounts: [AccountRoleV1; MAX_ACCOUNTS],
}

/// framed-tx-v2 additions: the v1 transaction format's message-level config.
///
/// All zero for legacy and v0 transactions. A value is meaningful only when its
/// bit is set in `config_mask` (see `V1_CONFIG_*`); absent values are 0.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TxExtV2 {
    /// Priority fee in lamports (not micro-lamports per CU).
    pub config_priority_fee_lamports: u64,
    pub config_mask: u32,
    pub config_compute_unit_limit: u32,
    pub config_loaded_accounts_data_size: u32,
    pub config_heap_size: u32,
}

impl TxExtV2 {
    pub const fn zero() -> Self {
        Self {
            config_priority_fee_lamports: 0,
            config_mask: 0,
            config_compute_unit_limit: 0,
            config_loaded_accounts_data_size: 0,
            config_heap_size: 0,
        }
    }
}

/// framed-tx-v2 encoding. `base` has exactly the framed-tx-v1 layout and
/// meaning (with `schema_id = 2` and `version = 1` for the v1 format).
#[repr(C)]
pub struct EncodedTxV2 {
    pub base: EncodedTxV1,
    pub ext: TxExtV2,
}

impl EncodedTxV2 {
    /// Heap-allocate a zeroed encoding without building it on the stack.
    pub fn boxed() -> Box<Self> {
        let mut boxed = Box::<Self>::new_uninit();
        // Safety: every field is an integer or an array of integers. The all-zero
        // bit pattern is a valid `EncodedTxV2`.
        unsafe {
            core::ptr::write_bytes(boxed.as_mut_ptr(), 0, 1);
            boxed.assume_init()
        }
    }
}

/// Per-instruction framing kept in scratch so the feature pass does not rescan
/// the message header. Opaque to C callers.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct IxMeta {
    pub accounts_offset: u32,
    pub accounts_len: u32,
    pub data_offset: u32,
    pub data_len: u32,
    pub program_index: u8,
    pub _pad: [u8; 3],
}

/// Caller-provided working memory. Reusable across encodes.
///
/// The Rust API requires a valid (initialized) `Scratch`. Each encode overwrites
/// the stamp prefix it uses, so a scratch left dirty by a previous encode is safe.
/// The C ABI zeroes the whole scratch on entry because C memory may be uninitialized.
#[repr(C, align(8))]
pub struct Scratch {
    pub(crate) ixs: [IxMeta; MAX_INSTRUCTIONS],
    pub(crate) seen_gen: [u32; MAX_ACCOUNTS],
    pub(crate) writable_gen: [u32; MAX_ACCOUNTS],
    pub(crate) dup_gen: [u32; MAX_ACCOUNTS],
}

impl TxHeaderV1 {
    pub const fn zero() -> Self {
        Self {
            requested_cu_price: 0,
            program_sequence_fingerprint: 0,
            instruction_structure_fingerprint: 0,
            topology_fingerprint: 0,
            schema_id: 0,
            raw_len: 0,
            total_instruction_account_refs: 0,
            reused_account_ref_count: 0,
            total_instruction_data_bytes: 0,
            max_instruction_data_len: 0,
            min_instruction_data_len: 0,
            compute_budget_flags: 0,
            requested_cu_limit: 0,
            requested_heap_bytes: 0,
            requested_loaded_accounts_data_size: 0,
            deprecated_requested_units: 0,
            deprecated_additional_fee: 0,
            signature_count: 0,
            required_signature_count: 0,
            readonly_signed_count: 0,
            readonly_unsigned_count: 0,
            static_account_count: 0,
            total_account_reference_count: 0,
            static_writable_count: 0,
            static_readonly_count: 0,
            static_signer_count: 0,
            instruction_count: 0,
            unique_program_reference_count: 0,
            unique_instruction_account_refs: 0,
            alt_lookup_count: 0,
            alt_loaded_writable_count: 0,
            alt_loaded_readonly_count: 0,
            compute_budget_instruction_count: 0,
            compute_budget_unknown_count: 0,
            compute_budget_malformed_count: 0,
            compute_budget_first_index: 0,
            compute_budget_last_index: 0,
            max_account_usage_count: 0,
            max_writable_account_usage_count: 0,
            instructions_with_shared_accounts: 0,
            instructions_with_shared_writable_accounts: 0,
            accounts_used_by_multiple_instructions: 0,
            writable_accounts_used_by_multiple_instructions: 0,
            adjacent_pairs_sharing_account: 0,
            adjacent_pairs_sharing_writable: 0,
            repeated_program_invocation_count: 0,
            version: 0,
            uses_alt: 0,
            program_repeated: 0,
            _pad: [0; 7],
        }
    }
}

impl InstructionFeaturesV1 {
    pub const fn zero() -> Self {
        Self {
            program_identity_hash64: 0,
            program_alt_identity_hash64: 0,
            structural_family_hash64: 0,
            writable_position_pattern: 0,
            signer_position_pattern: 0,
            prefix8: 0,
            prefix16: [0; 16],
            head: [0; 32],
            data_offset: 0,
            data_len: 0,
            accounts_offset: 0,
            accounts_len: 0,
            program_key_offset: 0,
            prefix4: 0,
            position: 0,
            account_count: 0,
            signer_account_count: 0,
            writable_account_count: 0,
            readonly_account_count: 0,
            static_account_ref_count: 0,
            alt_account_ref_count: 0,
            reused_account_count: 0,
            reused_writable_account_count: 0,
            alt_table_ordinal: 0,
            prefix2: 0,
            program_ref_kind: 0,
            program_account_index: 0,
            alt_address_index: 0,
            alt_role: 0,
            prefix_flags: 0,
            prefix1: 0,
            head_len: 0,
            data_len_class: 0,
            position_pattern_complete: 0,
            program_identity_available: 0,
        }
    }
}

impl AccountRoleV1 {
    pub const fn zero() -> Self {
        Self {
            identity_hash64: 0,
            alt_identity_hash64: 0,
            static_pubkey_offset: 0,
            alt_table_pubkey_offset: 0,
            expanded_index: 0,
            static_index: 0,
            alt_table_ordinal: 0,
            usage_count: 0,
            writable_usage_count: 0,
            program_usage_count: 0,
            first_instruction_seen: 0,
            last_instruction_seen: 0,
            origin: 0,
            signer: 0,
            writable: 0,
            readonly: 0,
            fee_payer: 0,
            used_as_program: 0,
            used_as_instruction_account: 0,
            alt_address_index: 0,
            has_static_identity: 0,
            static_pubkey_len: 0,
            alt_table_pubkey_len: 0,
            _pad: [0; 5],
        }
    }
}

impl EncodedTxV1 {
    /// Heap-allocate a zeroed encoding without building the 128 KiB value on the stack.
    pub fn boxed() -> Box<Self> {
        let mut boxed = Box::<Self>::new_uninit();
        // Safety: every field is an integer or an array of integers. The all-zero
        // bit pattern is a valid `EncodedTxV1`.
        unsafe {
            core::ptr::write_bytes(boxed.as_mut_ptr(), 0, 1);
            boxed.assume_init()
        }
    }
}

impl Scratch {
    pub fn new() -> Self {
        Self {
            ixs: [IxMeta {
                accounts_offset: 0,
                accounts_len: 0,
                data_offset: 0,
                data_len: 0,
                program_index: 0,
                _pad: [0; 3],
            }; MAX_INSTRUCTIONS],
            seen_gen: [0; MAX_ACCOUNTS],
            writable_gen: [0; MAX_ACCOUNTS],
            dup_gen: [0; MAX_ACCOUNTS],
        }
    }
}

impl Default for Scratch {
    fn default() -> Self {
        Self::new()
    }
}

/// Number of input bytes copied into the encoding on success.
///
/// The only copy is each instruction's data head (at most 32 bytes). Pubkeys
/// are hashed in place. Prefix integers are decoded from the same bytes.
pub fn copied_input_bytes(out: &EncodedTxV1) -> u64 {
    let n = out.header.instruction_count as usize;
    let mut sum = 0u64;
    for ix in &out.instructions[..n] {
        sum += ix.head_len as u64;
    }
    sum
}

/// Write the canonical populated image. Returns the number of bytes written.
///
/// This image is the historical/live equality contract: the same raw bytes must
/// produce the same canonical bytes.
pub fn write_canonical(out: &EncodedTxV1, dst: &mut [u8]) -> Option<usize> {
    let n = out.header.instruction_count as usize;
    let m = out.header.total_account_reference_count as usize;
    if n > MAX_INSTRUCTIONS || m > MAX_ACCOUNTS {
        return None;
    }
    let need = canonical_len(n, m);
    if dst.len() < need {
        return None;
    }
    let mut at = 0usize;
    at = write_pod(&out.header, dst, at);
    for ix in &out.instructions[..n] {
        at = write_pod(ix, dst, at);
    }
    for acct in &out.accounts[..m] {
        at = write_pod(acct, dst, at);
    }
    Some(at)
}

pub fn canonical_len(instruction_count: usize, account_count: usize) -> usize {
    core::mem::size_of::<TxHeaderV1>()
        + instruction_count * core::mem::size_of::<InstructionFeaturesV1>()
        + account_count * core::mem::size_of::<AccountRoleV1>()
}

fn write_pod<T>(value: &T, dst: &mut [u8], at: usize) -> usize {
    let n = core::mem::size_of::<T>();
    // Safety: `T` is a repr(C) integer record. The caller checked `dst` length.
    let src = unsafe { core::slice::from_raw_parts(value as *const T as *const u8, n) };
    dst[at..at + n].copy_from_slice(src);
    at + n
}
