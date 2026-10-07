//! Per-instruction structural features, prefixes, and program reference kind.

use crate::compute_budget::{self, CbAccum};
use crate::fingerprint::{data_len_class, structural_family, SIGNER_PATTERN_IV, WRITABLE_PATTERN_IV};
use crate::limits::{
    ALT_ROLE_NONE, ALT_ROLE_READONLY, ALT_ROLE_WRITABLE, INDEX_NONE, INSTRUCTION_HEAD_LEN,
    ORIGIN_ALT_READONLY, ORIGIN_ALT_WRITABLE, ORIGIN_STATIC, PREFIX_1, PREFIX_16, PREFIX_2, PREFIX_4,
    PREFIX_8, PROGRAM_ALT_REF, PROGRAM_STATIC, PROGRAM_UNRESOLVED,
};
use crate::status::Status;
use crate::view::{AccountRoleV1, InstructionFeaturesV1, IxMeta};
use crate::fingerprint::mix64;

#[inline]
fn set_bit(bits: &mut [u64; 4], idx: usize) {
    bits[idx >> 6] |= 1u64 << (idx & 63);
}

pub(crate) fn intersects(a: &[u64; 4], b: &[u64; 4]) -> bool {
    (a[0] & b[0]) | (a[1] & b[1]) | (a[2] & b[2]) | (a[3] & b[3]) != 0
}

fn read_u16(data: &[u8]) -> u16 {
    u16::from_le_bytes([data[0], data[1]])
}

fn read_u32(data: &[u8]) -> u32 {
    u32::from_le_bytes([data[0], data[1], data[2], data[3]])
}

fn read_u64(data: &[u8]) -> u64 {
    let mut lane = [0u8; 8];
    lane.copy_from_slice(&data[..8]);
    u64::from_le_bytes(lane)
}

fn fill_prefixes(data: &[u8], ix: &mut InstructionFeaturesV1) {
    let n = data.len().min(INSTRUCTION_HEAD_LEN);
    let mut head = [0u8; INSTRUCTION_HEAD_LEN];
    if n > 0 {
        head[..n].copy_from_slice(&data[..n]);
    }
    ix.head = head;
    ix.head_len = n as u8;
    ix.prefix_flags = 0;
    ix.prefix1 = 0;
    ix.prefix2 = 0;
    ix.prefix4 = 0;
    ix.prefix8 = 0;
    ix.prefix16 = [0; 16];
    if data.len() >= 1 {
        ix.prefix_flags |= PREFIX_1;
        ix.prefix1 = data[0];
    }
    if data.len() >= 2 {
        ix.prefix_flags |= PREFIX_2;
        ix.prefix2 = read_u16(data);
    }
    if data.len() >= 4 {
        ix.prefix_flags |= PREFIX_4;
        ix.prefix4 = read_u32(data);
    }
    if data.len() >= 8 {
        ix.prefix_flags |= PREFIX_8;
        ix.prefix8 = read_u64(data);
    }
    if data.len() >= 16 {
        ix.prefix_flags |= PREFIX_16;
        ix.prefix16.copy_from_slice(&data[..16]);
    }
}

fn note_instruction_seen(acct: &mut AccountRoleV1, ix_pos: u16) {
    if acct.first_instruction_seen == INDEX_NONE {
        acct.first_instruction_seen = ix_pos;
    }
    acct.last_instruction_seen = ix_pos;
}

pub(crate) struct IxAccum {
    pub any_bits: [u64; 4],
    pub writable_bits: [u64; 4],
    pub total_refs_delta: u32,
    pub data_len: u32,
}

/// Fill one instruction record and update per-account usage.
///
/// `gen` is the per-instruction stamp. `seen_gen` / `writable_gen` / `dup_gen`
/// are scratch arrays of length `total`, already cleared to 0 for this encode,
/// and `gen` starts at 1.
pub(crate) fn derive_instruction(
    raw: &[u8],
    meta: IxMeta,
    ix_pos: u16,
    gen: u32,
    static_len: usize,
    total: usize,
    accounts: &mut [AccountRoleV1],
    seen_gen: &mut [u32],
    writable_gen: &mut [u32],
    dup_gen: &mut [u32],
    cb: &mut CbAccum,
) -> Result<(InstructionFeaturesV1, IxAccum), Status> {
    let pidx = meta.program_index as usize;
    if pidx >= total {
        return Err(Status::Malformed);
    }

    let acc_off = meta.accounts_offset as usize;
    let acc_len = meta.accounts_len as usize;
    let acc_end = acc_off.checked_add(acc_len).ok_or(Status::Malformed)?;
    let data_off = meta.data_offset as usize;
    let data_len_usz = meta.data_len as usize;
    let data_end = data_off.checked_add(data_len_usz).ok_or(Status::Malformed)?;
    let Some(acc_bytes) = raw.get(acc_off..acc_end) else {
        return Err(Status::Malformed);
    };
    let Some(data) = raw.get(data_off..data_end) else {
        return Err(Status::Malformed);
    };

    let mut ix = InstructionFeaturesV1::zero();
    ix.position = ix_pos;
    ix.program_account_index = meta.program_index;
    ix.accounts_offset = meta.accounts_offset;
    ix.accounts_len = meta.accounts_len;
    ix.data_offset = meta.data_offset;
    ix.data_len = meta.data_len;
    ix.data_len_class = data_len_class(meta.data_len);
    ix.alt_table_ordinal = INDEX_NONE;
    fill_prefixes(data, &mut ix);

    match accounts[pidx].origin {
        ORIGIN_STATIC => {
            ix.program_ref_kind = PROGRAM_STATIC;
            ix.program_identity_hash64 = accounts[pidx].identity_hash64;
            ix.program_identity_available = 1;
            ix.program_key_offset = accounts[pidx].static_pubkey_offset;
        }
        ORIGIN_ALT_WRITABLE => {
            ix.program_ref_kind = PROGRAM_ALT_REF;
            ix.program_identity_available = 0;
            ix.program_alt_identity_hash64 = accounts[pidx].alt_identity_hash64;
            ix.program_key_offset = accounts[pidx].alt_table_pubkey_offset;
            ix.alt_table_ordinal = accounts[pidx].alt_table_ordinal;
            ix.alt_address_index = accounts[pidx].alt_address_index;
            ix.alt_role = ALT_ROLE_WRITABLE;
        }
        ORIGIN_ALT_READONLY => {
            ix.program_ref_kind = PROGRAM_ALT_REF;
            ix.program_identity_available = 0;
            ix.program_alt_identity_hash64 = accounts[pidx].alt_identity_hash64;
            ix.program_key_offset = accounts[pidx].alt_table_pubkey_offset;
            ix.alt_table_ordinal = accounts[pidx].alt_table_ordinal;
            ix.alt_address_index = accounts[pidx].alt_address_index;
            ix.alt_role = ALT_ROLE_READONLY;
        }
        _ => {
            ix.program_ref_kind = PROGRAM_UNRESOLVED;
            ix.program_identity_available = 0;
        }
    }

    if ix.program_ref_kind == PROGRAM_STATIC {
        let off = ix.program_key_offset as usize;
        if let Some(pk) = raw.get(off..off + 32) {
            if compute_budget::is_compute_budget_pubkey(pk) {
                compute_budget::observe(data, ix_pos, cb);
            }
        }
    }

    let mut any_bits = [0u64; 4];
    let mut writable_bits = [0u64; 4];
    let mut w_hash = WRITABLE_PATTERN_IV;
    let mut s_hash = SIGNER_PATTERN_IV;
    let mut wpat = 0u64;
    let mut spat = 0u64;
    let mut signer_refs = 0u16;
    let mut writable_refs = 0u16;
    let mut readonly_refs = 0u16;
    let mut static_refs = 0u16;
    let mut alt_refs = 0u16;
    let mut reused = 0u16;
    let mut reused_w = 0u16;

    for (pos, &idx_b) in acc_bytes.iter().enumerate() {
        let idx = idx_b as usize;
        if idx >= total {
            return Err(Status::Malformed);
        }
        let writable = accounts[idx].writable == 1;
        let signer = accounts[idx].signer == 1;
        if writable {
            w_hash = mix64(w_hash, pos as u64);
            writable_refs = writable_refs.saturating_add(1);
            if pos < 64 {
                wpat |= 1u64 << pos;
            }
        } else {
            readonly_refs = readonly_refs.saturating_add(1);
        }
        if signer {
            s_hash = mix64(s_hash, pos as u64);
            signer_refs = signer_refs.saturating_add(1);
            if pos < 64 {
                spat |= 1u64 << pos;
            }
        }
        if idx < static_len {
            static_refs = static_refs.saturating_add(1);
        } else {
            alt_refs = alt_refs.saturating_add(1);
        }

        if seen_gen[idx] != gen {
            seen_gen[idx] = gen;
            accounts[idx].usage_count = accounts[idx].usage_count.saturating_add(1);
            accounts[idx].used_as_instruction_account = 1;
            note_instruction_seen(&mut accounts[idx], ix_pos);
        } else if dup_gen[idx] != gen {
            dup_gen[idx] = gen;
            reused = reused.saturating_add(1);
            if writable {
                reused_w = reused_w.saturating_add(1);
            }
        }
        if writable && writable_gen[idx] != gen {
            writable_gen[idx] = gen;
            accounts[idx].writable_usage_count = accounts[idx].writable_usage_count.saturating_add(1);
        }
        set_bit(&mut any_bits, idx);
        if writable {
            set_bit(&mut writable_bits, idx);
        }
    }

    if seen_gen[pidx] != gen {
        seen_gen[pidx] = gen;
        accounts[pidx].usage_count = accounts[pidx].usage_count.saturating_add(1);
        note_instruction_seen(&mut accounts[pidx], ix_pos);
    }
    accounts[pidx].program_usage_count = accounts[pidx].program_usage_count.saturating_add(1);
    accounts[pidx].used_as_program = 1;
    set_bit(&mut any_bits, pidx);

    let list_len = acc_bytes.len();
    ix.account_count = list_len as u16;
    ix.signer_account_count = signer_refs;
    ix.writable_account_count = writable_refs;
    ix.readonly_account_count = readonly_refs;
    ix.static_account_ref_count = static_refs;
    ix.alt_account_ref_count = alt_refs;
    ix.reused_account_count = reused;
    ix.reused_writable_account_count = reused_w;
    ix.writable_position_pattern = wpat;
    ix.signer_position_pattern = spat;
    ix.position_pattern_complete = u8::from(list_len <= 64);
    if ix.alt_role == 0 && ix.program_ref_kind != PROGRAM_ALT_REF {
        ix.alt_role = ALT_ROLE_NONE;
    }
    ix.structural_family_hash64 = structural_family(
        list_len as u64,
        w_hash,
        s_hash,
        ix.data_len_class as u64,
    );

    if list_len > u16::MAX as usize {
        return Err(Status::CapacityExceeded);
    }

    Ok((
        ix,
        IxAccum {
            any_bits,
            writable_bits,
            total_refs_delta: list_len as u32,
            data_len: meta.data_len,
        },
    ))
}
