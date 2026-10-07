//! Deterministic non-cryptographic mixes used by V1 identity and fingerprints.
//!
//! These constants are part of `framed-tx-v1`. Changing them is a schema break.

/// Golden-ratio odd constant used by [`mix64`].
pub const MIX_CONST: u64 = 0x9E3779B185EBCA87;

/// Fractional bits of pi. IV for [`identity_hash64`].
pub const IDENTITY_IV: u64 = 0x243F6A8885A308D3;

pub const WRITABLE_PATTERN_IV: u64 = 0x6A09E667F3BCC909;
pub const SIGNER_PATTERN_IV: u64 = 0xBB67AE8584CAA73B;
pub const STRUCTURAL_FAMILY_IV: u64 = 0x3C6EF372FE94F82B;
pub const PROGRAM_SEQUENCE_IV: u64 = 0xA54FF53A5F1D36F1;
pub const STRUCTURE_FP_IV: u64 = 0x510E527FADE682D1;
pub const TOPOLOGY_FP_IV: u64 = 0x9B05688C2B3E6C1F;

/// Lane multiplier inside [`identity_hash64`].
pub const IDENTITY_MUL: u64 = 0xBF58476D1CE4E5B9;
pub const IDENTITY_MUL2: u64 = 0x94D049BB133111EB;

#[inline(always)]
pub fn mix64(h: u64, v: u64) -> u64 {
    let mut x = h.wrapping_add(v).wrapping_mul(MIX_CONST);
    x ^= x >> 30;
    x
}

/// Stable 64-bit identity of an arbitrary byte string.
///
/// Not a cryptographic hash. 32-byte pubkeys take four unaligned little-endian
/// lanes. The function is pure and has no lookup tables.
pub fn identity_hash64(bytes: &[u8]) -> u64 {
    let mut h = IDENTITY_IV;
    let mut off = 0usize;
    while off + 8 <= bytes.len() {
        let mut lane = [0u8; 8];
        lane.copy_from_slice(&bytes[off..off + 8]);
        let x = u64::from_le_bytes(lane);
        h ^= x.wrapping_mul(IDENTITY_MUL);
        h = h.rotate_left(27).wrapping_mul(IDENTITY_MUL2);
        off += 8;
    }
    if off < bytes.len() {
        let mut x = 0u64;
        let mut shift = 0u32;
        for b in &bytes[off..] {
            x |= (*b as u64) << shift;
            shift += 8;
        }
        h ^= x.wrapping_mul(IDENTITY_MUL);
        h = h.rotate_left(27).wrapping_mul(IDENTITY_MUL2);
    }
    h ^= bytes.len() as u64;
    h
}

#[inline(always)]
pub fn structural_family(account_count: u64, writable_pattern: u64, signer_pattern: u64, data_len_class: u64) -> u64 {
    let mut h = STRUCTURAL_FAMILY_IV;
    h = mix64(h, account_count);
    h = mix64(h, writable_pattern);
    h = mix64(h, signer_pattern);
    h = mix64(h, data_len_class);
    h
}

/// Identity of an unresolved ALT reference. Mixes the on-wire lookup-table
/// pubkey hash with the table ordinal, address index, and role. This is not
/// the loaded account pubkey.
#[inline(always)]
pub fn alt_identity(table_pubkey_hash: u64, ordinal: u64, address_index: u64, role: u64) -> u64 {
    let mut h = table_pubkey_hash;
    h = mix64(h, ordinal);
    h = mix64(h, address_index);
    h = mix64(h, role);
    h
}

/// Coarse class of an instruction data length. The exact length is stored
/// separately; this class is only an input to the structural family.
#[inline(always)]
pub fn data_len_class(len: u32) -> u8 {
    match len {
        0 => 0,
        1..=2 => 1,
        3..=4 => 2,
        5..=8 => 3,
        9..=16 => 4,
        17..=32 => 5,
        33..=64 => 6,
        65..=128 => 7,
        129..=256 => 8,
        257..=512 => 9,
        513..=1024 => 10,
        _ => 11,
    }
}
