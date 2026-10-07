//! Fixed capacities for caller-provided scratch and output.
//!
//! These are protocol ceilings, not observed pilot maxima. Exceeding one returns
//! [`crate::Status::CapacityExceeded`] and never truncates.

/// Compiled instruction account indexes and program-id indexes are `u8`,
/// so a message can address at most 256 accounts (static keys plus loaded
/// address-lookup entries). This matches Solana message sanitization.
pub const MAX_ACCOUNTS: usize = 256;

/// Upper bound on top-level compiled instructions stored by V1.
///
/// A compiled instruction is at least 3 bytes. Solana packet payloads are
/// 1232 bytes (`1280 - 40 - 8`, the IPv6 minimum MTU minus headers), which
/// caps a pathological packet near ~387 empty instructions. 768 covers that
/// packet and a modest amount of headroom beyond it. Larger messages fail
/// closed instead of being truncated.
pub const MAX_INSTRUCTIONS: usize = 768;

/// Each address-lookup descriptor is at least 34 bytes (32-byte table pubkey
/// plus two shortvec lengths). 64 descriptors are already larger than a
/// 1232-byte packet, so every packet-legal transaction fits.
pub const MAX_LOOKUP_TABLES: usize = 64;

/// Legacy messages are tagged with this version feature value.
/// The wire format has no explicit legacy version byte; the high bit of the
/// first message byte is clear.
pub const VERSION_LEGACY: u8 = 255;

/// Versioned message v0.
pub const VERSION_V0: u8 = 0;

/// V1 transaction format (`framed-tx-v2` only). The wire starts with
/// [`V1_PREFIX`]; signatures trail the message.
pub const VERSION_V1: u8 = 1;

/// First byte of a v1-format transaction (`MESSAGE_VERSION_PREFIX | 1`).
pub const V1_PREFIX: u8 = 0x81;

/// V1 transaction-config mask bits (`TransactionConfigMask`). The priority fee
/// uses both bit 0 and bit 1; exactly one of them set is malformed.
pub const V1_CONFIG_PRIORITY_FEE: u32 = 0b11;
pub const V1_CONFIG_COMPUTE_UNIT_LIMIT: u32 = 0b100;
pub const V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE: u32 = 0b1000;
pub const V1_CONFIG_HEAP_SIZE: u32 = 0b10000;
pub const V1_CONFIG_KNOWN_BITS: u32 = 0b11111;

/// `feature_schema` string. Frozen for any model trained on V1.
pub const FEATURE_SCHEMA: &str = "framed-tx-v1";

/// Numeric schema id written into [`crate::TxHeaderV1::schema_id`].
pub const FEATURE_SCHEMA_ID: u32 = 1;

/// `feature_schema` of [`crate::encode_transaction_v2`]: framed-tx-v1 plus the
/// v1 transaction format and its message-level config.
pub const FEATURE_SCHEMA_V2: &str = "framed-tx-v2";

/// `schema_id` written by [`crate::encode_transaction_v2`].
pub const FEATURE_SCHEMA_V2_ID: u32 = 2;

/// Sentinel for "no instruction index" and "not a static/ALT ordinal".
pub const INDEX_NONE: u16 = u16::MAX;

pub const ORIGIN_STATIC: u8 = 1;
pub const ORIGIN_ALT_WRITABLE: u8 = 2;
pub const ORIGIN_ALT_READONLY: u8 = 3;

pub const PROGRAM_STATIC: u8 = 1;
pub const PROGRAM_ALT_REF: u8 = 2;
pub const PROGRAM_UNRESOLVED: u8 = 3;

pub const ALT_ROLE_NONE: u8 = 0;
pub const ALT_ROLE_WRITABLE: u8 = 1;
pub const ALT_ROLE_READONLY: u8 = 2;

pub const PREFIX_1: u8 = 1 << 0;
pub const PREFIX_2: u8 = 1 << 1;
pub const PREFIX_4: u8 = 1 << 2;
pub const PREFIX_8: u8 = 1 << 3;
pub const PREFIX_16: u8 = 1 << 4;

/// Instruction data head copied into the output. Not a protocol limit.
pub const INSTRUCTION_HEAD_LEN: usize = 32;

pub const GROUP_STRUCTURAL: &str = "structural";
pub const GROUP_IDENTITY: &str = "identity";
pub const GROUP_RAW: &str = "raw";
pub const GROUP_META: &str = "meta";
