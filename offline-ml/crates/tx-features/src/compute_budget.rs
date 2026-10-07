//! Exact Compute Budget program layouts.
//!
//! Discriminant is a single `u8`, followed by a fixed little-endian payload.
//! This is the layout the runtime has executed for years (not bincode's `u32`
//! enum discriminant). A known discriminant with any other length is recorded
//! as a malformed payload and is not decoded into a numeric field. An unknown
//! discriminant stays unknown. Nothing is inferred from a prefix.

use crate::fingerprint::identity_hash64;

/// `ComputeBudget111111111111111111111111111111`
pub const COMPUTE_BUDGET_PROGRAM_ID: [u8; 32] = [
    0x03, 0x06, 0x46, 0x6f, 0xe5, 0x21, 0x17, 0x32, 0xff, 0xec, 0xad, 0xba, 0x72, 0xc3, 0x9b, 0xe7,
    0xbc, 0x8c, 0xe5, 0xbb, 0xc5, 0xf7, 0x12, 0x6b, 0x2c, 0x43, 0x9b, 0x3a, 0x40, 0x00, 0x00, 0x00,
];

pub const CB_LIMIT: u32 = 1 << 0;
pub const CB_PRICE: u32 = 1 << 1;
pub const CB_HEAP: u32 = 1 << 2;
pub const CB_DATA_SIZE: u32 = 1 << 3;
pub const CB_DEPRECATED: u32 = 1 << 4;
pub const CB_DUP_LIMIT: u32 = 1 << 5;
pub const CB_DUP_PRICE: u32 = 1 << 6;
pub const CB_DUP_HEAP: u32 = 1 << 7;
pub const CB_DUP_DATA_SIZE: u32 = 1 << 8;
pub const CB_DUP_DEPRECATED: u32 = 1 << 9;
pub const CB_UNKNOWN: u32 = 1 << 10;
pub const CB_MALFORMED_PAYLOAD: u32 = 1 << 11;

#[derive(Clone, Copy, Debug)]
pub(crate) struct CbAccum {
    pub flags: u32,
    pub count: u16,
    pub unknown: u16,
    pub malformed: u16,
    pub first: u16,
    pub last: u16,
    pub limit: u32,
    pub price: u64,
    pub heap: u32,
    pub data_size: u32,
    pub deprecated_units: u32,
    pub deprecated_fee: u32,
}

impl CbAccum {
    pub(crate) const fn new() -> Self {
        Self {
            flags: 0,
            count: 0,
            unknown: 0,
            malformed: 0,
            first: crate::limits::INDEX_NONE,
            last: crate::limits::INDEX_NONE,
            limit: 0,
            price: 0,
            heap: 0,
            data_size: 0,
            deprecated_units: 0,
            deprecated_fee: 0,
        }
    }
}

#[inline]
fn note_dup(flags: &mut u32, present: u32, dup: u32) {
    if *flags & present != 0 {
        *flags |= dup;
    }
    *flags |= present;
}

/// Record one Compute Budget instruction payload.
///
/// The last well-formed value of each variant wins. Duplicates set a flag and
/// do not erase the fact that an earlier value existed.
pub(crate) fn observe(data: &[u8], ix_pos: u16, acc: &mut CbAccum) {
    if acc.count == 0 {
        acc.first = ix_pos;
    }
    acc.last = ix_pos;
    acc.count = acc.count.saturating_add(1);

    let Some((&disc, rest)) = data.split_first() else {
        acc.unknown = acc.unknown.saturating_add(1);
        acc.flags |= CB_UNKNOWN;
        return;
    };

    match disc {
        0 if rest.len() == 8 => {
            note_dup(&mut acc.flags, CB_DEPRECATED, CB_DUP_DEPRECATED);
            acc.deprecated_units = u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]);
            acc.deprecated_fee = u32::from_le_bytes([rest[4], rest[5], rest[6], rest[7]]);
        }
        1 if rest.len() == 4 => {
            note_dup(&mut acc.flags, CB_HEAP, CB_DUP_HEAP);
            acc.heap = u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]);
        }
        2 if rest.len() == 4 => {
            note_dup(&mut acc.flags, CB_LIMIT, CB_DUP_LIMIT);
            acc.limit = u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]);
        }
        3 if rest.len() == 8 => {
            note_dup(&mut acc.flags, CB_PRICE, CB_DUP_PRICE);
            let mut lane = [0u8; 8];
            lane.copy_from_slice(rest);
            acc.price = u64::from_le_bytes(lane);
        }
        4 if rest.len() == 4 => {
            note_dup(&mut acc.flags, CB_DATA_SIZE, CB_DUP_DATA_SIZE);
            acc.data_size = u32::from_le_bytes([rest[0], rest[1], rest[2], rest[3]]);
        }
        0 | 1 | 2 | 3 | 4 => {
            acc.malformed = acc.malformed.saturating_add(1);
            acc.flags |= CB_MALFORMED_PAYLOAD;
        }
        _ => {
            acc.unknown = acc.unknown.saturating_add(1);
            acc.flags |= CB_UNKNOWN;
        }
    }
}

#[inline]
pub(crate) fn is_compute_budget_pubkey(pubkey: &[u8]) -> bool {
    pubkey == COMPUTE_BUDGET_PROGRAM_ID
}

/// Precomputed so tests can lock the identity of the well-known program id.
pub fn compute_budget_identity_hash64() -> u64 {
    identity_hash64(&COMPUTE_BUDGET_PROGRAM_ID)
}
