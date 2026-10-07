//! Allocation-free encoder for Solana framed transactions.
//!
//! Historical JSONL tooling and the live searcher both call
//! [`encode_transaction_v1`]. There is no second feature implementation.
//!
//! The crate does not know about economic labels, account state, or RPC.
//! Everything in [`EncodedTxV1`] is derived from the raw serialized transaction.

#![allow(clippy::too_many_arguments)]

mod accounts;
mod compute_budget;
mod features;
mod fingerprint;
mod instruction;
mod limits;
mod shortvec;
mod status;
mod view;
mod wire;

pub mod ffi;

#[cfg(feature = "bench-harness")]
pub mod bench_harness;

#[cfg(any(test, feature = "reference"))]
pub mod reference;

#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_v2;

pub use compute_budget::{
    compute_budget_identity_hash64, CB_DATA_SIZE, CB_DEPRECATED, CB_DUP_DATA_SIZE, CB_DUP_DEPRECATED,
    CB_DUP_HEAP, CB_DUP_LIMIT, CB_DUP_PRICE, CB_HEAP, CB_LIMIT, CB_MALFORMED_PAYLOAD, CB_PRICE,
    CB_UNKNOWN, COMPUTE_BUDGET_PROGRAM_ID,
};
pub use features::{encode_transaction_v1, encode_transaction_v2};
#[cfg(feature = "phase-profile")]
pub use features::PHASE_CYCLES;
pub use fingerprint::{data_len_class, identity_hash64, mix64, IDENTITY_IV};
pub use limits::{
    FEATURE_SCHEMA, FEATURE_SCHEMA_ID, GROUP_IDENTITY, GROUP_META, GROUP_RAW, GROUP_STRUCTURAL,
    INDEX_NONE, INSTRUCTION_HEAD_LEN, MAX_ACCOUNTS, MAX_INSTRUCTIONS, MAX_LOOKUP_TABLES,
    ORIGIN_ALT_READONLY, ORIGIN_ALT_WRITABLE, ORIGIN_STATIC, PREFIX_1, PREFIX_16, PREFIX_2, PREFIX_4,
    PREFIX_8, PROGRAM_ALT_REF, PROGRAM_STATIC, PROGRAM_UNRESOLVED, VERSION_LEGACY, VERSION_V0,
    FEATURE_SCHEMA_V2, FEATURE_SCHEMA_V2_ID, V1_CONFIG_COMPUTE_UNIT_LIMIT, V1_CONFIG_HEAP_SIZE,
    V1_CONFIG_KNOWN_BITS, V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE, V1_CONFIG_PRIORITY_FEE, V1_PREFIX,
    VERSION_V1,
};
pub use status::Status;
pub use view::{
    copied_input_bytes, write_canonical, AccountRoleV1, EncodedTxV1, EncodedTxV2, InstructionFeaturesV1,
    Scratch, TxExtV2, TxHeaderV1,
};
