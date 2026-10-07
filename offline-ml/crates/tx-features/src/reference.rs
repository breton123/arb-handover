//! Slow reference encoder used only by tests.
//!
//! The byte walk below is intentionally separate from [`crate::wire`] and
//! [`crate::shortvec`]. It exists to catch optimized-parser mistakes. It may
//! allocate. Do not call it from the searcher.

use crate::compute_budget::{self, CbAccum};
use crate::fingerprint::{
    alt_identity, data_len_class, identity_hash64, mix64, structural_family, PROGRAM_SEQUENCE_IV,
    SIGNER_PATTERN_IV, STRUCTURE_FP_IV, TOPOLOGY_FP_IV, WRITABLE_PATTERN_IV,
};
use crate::limits::{
    ALT_ROLE_NONE, ALT_ROLE_READONLY, ALT_ROLE_WRITABLE, FEATURE_SCHEMA_ID, FEATURE_SCHEMA_V2_ID, INDEX_NONE,
    MAX_ACCOUNTS, VERSION_V1,
    MAX_INSTRUCTIONS, MAX_LOOKUP_TABLES, ORIGIN_ALT_READONLY, ORIGIN_ALT_WRITABLE, ORIGIN_STATIC,
    PREFIX_1, PREFIX_16, PREFIX_2, PREFIX_4, PREFIX_8, PROGRAM_ALT_REF, PROGRAM_STATIC,
    PROGRAM_UNRESOLVED, VERSION_LEGACY, VERSION_V0,
};
use crate::status::Status;
use crate::view::{AccountRoleV1, EncodedTxV1, EncodedTxV2, InstructionFeaturesV1, TxExtV2, TxHeaderV1};

struct OwnedIx {
    program: u8,
    accounts: Vec<u8>,
    data: Vec<u8>,
    accounts_offset: u32,
    data_offset: u32,
}

struct OwnedLookup {
    table_offset: u32,
    table: [u8; 32],
    writable: Vec<u8>,
    readonly: Vec<u8>,
}

struct OwnedTx {
    version: u8,
    signature_count: u16,
    required: u16,
    readonly_signed: u16,
    readonly_unsigned: u16,
    static_keys: Vec<[u8; 32]>,
    key_base: u32,
    instructions: Vec<OwnedIx>,
    lookups: Vec<OwnedLookup>,
    /// framed-tx-v2: loaded rows are all tables' writable, then all readonly.
    /// framed-tx-v1 (false) interleaves per table (documented erratum).
    runtime_alt_order: bool,
}

fn read_short(raw: &[u8], i: &mut usize) -> Result<usize, Status> {
    let mut value = 0usize;
    let mut size = 0usize;
    loop {
        if *i >= raw.len() {
            return Err(Status::Malformed);
        }
        let elem = raw[*i];
        *i += 1;
        value |= ((elem & 0x7f) as usize) << (size * 7);
        size += 1;
        if elem & 0x80 == 0 {
            break;
        }
        if size >= 3 {
            return Err(Status::Malformed);
        }
    }
    if value > u16::MAX as usize {
        return Err(Status::Malformed);
    }
    Ok(value)
}

fn take<'a>(raw: &'a [u8], i: &mut usize, n: usize) -> Result<&'a [u8], Status> {
    let end = i.checked_add(n).ok_or(Status::Malformed)?;
    if end > raw.len() {
        return Err(Status::Malformed);
    }
    let s = &raw[*i..end];
    *i = end;
    Ok(s)
}

fn parse_owned(raw: &[u8]) -> Result<OwnedTx, Status> {
    if raw.len() > u32::MAX as usize {
        return Err(Status::CapacityExceeded);
    }
    let mut i = 0usize;
    let sig_count = read_short(raw, &mut i)?;
    let sig_bytes = sig_count.checked_mul(64).ok_or(Status::Malformed)?;
    take(raw, &mut i, sig_bytes)?;

    if i >= raw.len() {
        return Err(Status::Malformed);
    }
    let b0 = raw[i];
    i += 1;
    let (version, required) = if b0 & 0x80 != 0 {
        let ver = b0 & 0x7f;
        if ver != 0 {
            return Err(Status::UnsupportedVersion);
        }
        if i >= raw.len() {
            return Err(Status::Malformed);
        }
        let req = raw[i];
        i += 1;
        (VERSION_V0, req as usize)
    } else {
        (VERSION_LEGACY, b0 as usize)
    };
    if i + 2 > raw.len() {
        return Err(Status::Malformed);
    }
    let ro_s = raw[i] as usize;
    let ro_u = raw[i + 1] as usize;
    i += 2;

    if sig_count != required {
        return Err(Status::Malformed);
    }

    let static_len = read_short(raw, &mut i)?;
    if static_len > MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }
    if ro_s > required || required > static_len || ro_u > static_len - required {
        return Err(Status::Malformed);
    }
    let key_base = i;
    let mut static_keys = Vec::with_capacity(static_len);
    for _ in 0..static_len {
        let pk = take(raw, &mut i, 32)?;
        let mut key = [0u8; 32];
        key.copy_from_slice(pk);
        static_keys.push(key);
    }
    take(raw, &mut i, 32)?;

    let ix_count = read_short(raw, &mut i)?;
    if ix_count > MAX_INSTRUCTIONS {
        return Err(Status::CapacityExceeded);
    }
    let mut instructions = Vec::with_capacity(ix_count);
    for _ in 0..ix_count {
        if i >= raw.len() {
            return Err(Status::Malformed);
        }
        let program = raw[i];
        i += 1;
        let acc_len = read_short(raw, &mut i)?;
        let accounts_offset = u32::try_from(i).map_err(|_| Status::CapacityExceeded)?;
        let acc = take(raw, &mut i, acc_len)?;
        let data_len = read_short(raw, &mut i)?;
        let data_offset = u32::try_from(i).map_err(|_| Status::CapacityExceeded)?;
        let data = take(raw, &mut i, data_len)?;
        instructions.push(OwnedIx {
            program,
            accounts: acc.to_vec(),
            data: data.to_vec(),
            accounts_offset,
            data_offset,
        });
    }

    let mut lookups = Vec::new();
    if version == VERSION_V0 {
        let lookup_count = read_short(raw, &mut i)?;
        if lookup_count > MAX_LOOKUP_TABLES {
            return Err(Status::CapacityExceeded);
        }
        let mut running = static_len;
        for _ in 0..lookup_count {
            let table_offset = u32::try_from(i).map_err(|_| Status::CapacityExceeded)?;
            let table_bytes = take(raw, &mut i, 32)?;
            let mut table = [0u8; 32];
            table.copy_from_slice(table_bytes);
            let wl = read_short(raw, &mut i)?;
            if running.checked_add(wl).ok_or(Status::Malformed)? > MAX_ACCOUNTS {
                return Err(Status::CapacityExceeded);
            }
            let w = take(raw, &mut i, wl)?.to_vec();
            running += wl;
            let rl = read_short(raw, &mut i)?;
            if running.checked_add(rl).ok_or(Status::Malformed)? > MAX_ACCOUNTS {
                return Err(Status::CapacityExceeded);
            }
            let r = take(raw, &mut i, rl)?.to_vec();
            running += rl;
            lookups.push(OwnedLookup {
                table_offset,
                table,
                writable: w,
                readonly: r,
            });
        }
    }

    if i != raw.len() {
        return Err(Status::Malformed);
    }

    Ok(OwnedTx {
        version,
        signature_count: sig_count as u16,
        required: required as u16,
        readonly_signed: ro_s as u16,
        readonly_unsigned: ro_u as u16,
        static_keys,
        key_base: key_base as u32,
        instructions,
        lookups,
        runtime_alt_order: false,
    })
}

fn static_writable(index: usize, required: usize, ro_s: usize, static_len: usize, ro_u: usize) -> bool {
    if index < required {
        index < required - ro_s
    } else {
        index < static_len - ro_u
    }
}

fn blank_account(expanded: u16) -> AccountRoleV1 {
    let mut a = AccountRoleV1::zero();
    a.expanded_index = expanded;
    a.static_index = INDEX_NONE;
    a.alt_table_ordinal = INDEX_NONE;
    a.first_instruction_seen = INDEX_NONE;
    a.last_instruction_seen = INDEX_NONE;
    a
}

fn le_u16(d: &[u8]) -> u16 {
    u16::from_le_bytes([d[0], d[1]])
}
fn le_u32(d: &[u8]) -> u32 {
    u32::from_le_bytes([d[0], d[1], d[2], d[3]])
}
fn le_u64(d: &[u8]) -> u64 {
    let mut b = [0u8; 8];
    b.copy_from_slice(&d[..8]);
    u64::from_le_bytes(b)
}

fn featurize(raw: &[u8], tx: &OwnedTx, out: &mut EncodedTxV1) -> Result<(), Status> {
    let required = tx.required as usize;
    let ro_s = tx.readonly_signed as usize;
    let ro_u = tx.readonly_unsigned as usize;
    let static_len = tx.static_keys.len();

    let mut expanded: Vec<AccountRoleV1> = Vec::new();
    for (i, key) in tx.static_keys.iter().enumerate() {
        let writable = static_writable(i, required, ro_s, static_len, ro_u);
        let mut a = blank_account(i as u16);
        a.origin = ORIGIN_STATIC;
        a.static_index = i as u16;
        a.signer = u8::from(i < required);
        a.writable = u8::from(writable);
        a.readonly = u8::from(!writable);
        a.fee_payer = u8::from(i == 0);
        a.has_static_identity = 1;
        a.static_pubkey_len = 32;
        a.static_pubkey_offset = tx.key_base + (i as u32) * 32;
        a.identity_hash64 = identity_hash64(key);
        expanded.push(a);
    }
    let mut loaded_w = 0u16;
    let mut loaded_r = 0u16;
    // framed-tx-v2: every table's writable rows, then every table's readonly rows.
    // framed-tx-v1: table by table (erratum kept for the frozen schema).
    let order: Vec<(usize, bool)> = if tx.runtime_alt_order {
        (0..tx.lookups.len()).map(|o| (o, true)).chain((0..tx.lookups.len()).map(|o| (o, false))).collect()
    } else {
        (0..tx.lookups.len()).flat_map(|o| [(o, true), (o, false)]).collect()
    };
    for (ordinal, writable_rows) in order {
        let lookup = &tx.lookups[ordinal];
        let table_hash = identity_hash64(&lookup.table);
        let (wrows, rrows): (&[u8], &[u8]) = if writable_rows { (&lookup.writable, &[]) } else { (&[], &lookup.readonly) };
        for &addr in wrows {
            let mut a = blank_account(expanded.len() as u16);
            a.origin = ORIGIN_ALT_WRITABLE;
            a.writable = 1;
            a.alt_table_ordinal = ordinal as u16;
            a.alt_address_index = addr;
            a.alt_table_pubkey_offset = lookup.table_offset;
            a.alt_table_pubkey_len = 32;
            a.alt_identity_hash64 =
                alt_identity(table_hash, ordinal as u64, addr as u64, ORIGIN_ALT_WRITABLE as u64);
            expanded.push(a);
            loaded_w += 1;
        }
        for &addr in rrows {
            let mut a = blank_account(expanded.len() as u16);
            a.origin = ORIGIN_ALT_READONLY;
            a.readonly = 1;
            a.alt_table_ordinal = ordinal as u16;
            a.alt_address_index = addr;
            a.alt_table_pubkey_offset = lookup.table_offset;
            a.alt_table_pubkey_len = 32;
            a.alt_identity_hash64 =
                alt_identity(table_hash, ordinal as u64, addr as u64, ORIGIN_ALT_READONLY as u64);
            expanded.push(a);
            loaded_r += 1;
        }
    }
    if expanded.len() > MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }

    let mut cb = CbAccum::new();
    let mut total_refs = 0u32;
    let mut total_data = 0u32;
    let mut max_data = 0u32;
    let mut min_data = u32::MAX;
    let mut shared_ix = 0u16;
    let mut shared_w = 0u16;

    for (ix_i, ix) in tx.instructions.iter().enumerate() {
        let pidx = ix.program as usize;
        if pidx >= expanded.len() {
            return Err(Status::Malformed);
        }
        let mut rec = InstructionFeaturesV1::zero();
        rec.position = ix_i as u16;
        rec.program_account_index = ix.program;
        rec.accounts_offset = ix.accounts_offset;
        rec.accounts_len = ix.accounts.len() as u32;
        rec.data_offset = ix.data_offset;
        rec.data_len = ix.data.len() as u32;
        rec.data_len_class = data_len_class(rec.data_len);
        rec.alt_table_ordinal = INDEX_NONE;
        let nhead = ix.data.len().min(32);
        if nhead > 0 {
            rec.head[..nhead].copy_from_slice(&ix.data[..nhead]);
        }
        rec.head_len = nhead as u8;
        if ix.data.len() >= 1 {
            rec.prefix_flags |= PREFIX_1;
            rec.prefix1 = ix.data[0];
        }
        if ix.data.len() >= 2 {
            rec.prefix_flags |= PREFIX_2;
            rec.prefix2 = le_u16(&ix.data);
        }
        if ix.data.len() >= 4 {
            rec.prefix_flags |= PREFIX_4;
            rec.prefix4 = le_u32(&ix.data);
        }
        if ix.data.len() >= 8 {
            rec.prefix_flags |= PREFIX_8;
            rec.prefix8 = le_u64(&ix.data);
        }
        if ix.data.len() >= 16 {
            rec.prefix_flags |= PREFIX_16;
            rec.prefix16.copy_from_slice(&ix.data[..16]);
        }

        match expanded[pidx].origin {
            ORIGIN_STATIC => {
                rec.program_ref_kind = PROGRAM_STATIC;
                rec.program_identity_available = 1;
                rec.program_identity_hash64 = expanded[pidx].identity_hash64;
                rec.program_key_offset = expanded[pidx].static_pubkey_offset;
            }
            ORIGIN_ALT_WRITABLE => {
                rec.program_ref_kind = PROGRAM_ALT_REF;
                rec.program_alt_identity_hash64 = expanded[pidx].alt_identity_hash64;
                rec.program_key_offset = expanded[pidx].alt_table_pubkey_offset;
                rec.alt_table_ordinal = expanded[pidx].alt_table_ordinal;
                rec.alt_address_index = expanded[pidx].alt_address_index;
                rec.alt_role = ALT_ROLE_WRITABLE;
            }
            ORIGIN_ALT_READONLY => {
                rec.program_ref_kind = PROGRAM_ALT_REF;
                rec.program_alt_identity_hash64 = expanded[pidx].alt_identity_hash64;
                rec.program_key_offset = expanded[pidx].alt_table_pubkey_offset;
                rec.alt_table_ordinal = expanded[pidx].alt_table_ordinal;
                rec.alt_address_index = expanded[pidx].alt_address_index;
                rec.alt_role = ALT_ROLE_READONLY;
            }
            _ => {
                rec.program_ref_kind = PROGRAM_UNRESOLVED;
            }
        }
        if rec.program_ref_kind == PROGRAM_STATIC {
            let off = rec.program_key_offset as usize;
            if raw.get(off..off + 32) == Some(compute_budget::COMPUTE_BUDGET_PROGRAM_ID.as_slice()) {
                compute_budget::observe(&ix.data, ix_i as u16, &mut cb);
            }
        }

        let mut seen_here = vec![0u8; expanded.len()];
        let mut w_hash = WRITABLE_PATTERN_IV;
        let mut s_hash = SIGNER_PATTERN_IV;
        let mut this_w = vec![false; expanded.len()];

        for (pos, &idx_b) in ix.accounts.iter().enumerate() {
            let idx = idx_b as usize;
            if idx >= expanded.len() {
                return Err(Status::Malformed);
            }
            let writable = expanded[idx].writable == 1;
            let signer = expanded[idx].signer == 1;
            if writable {
                w_hash = mix64(w_hash, pos as u64);
                rec.writable_account_count += 1;
                if pos < 64 {
                    rec.writable_position_pattern |= 1u64 << pos;
                }
            } else {
                rec.readonly_account_count += 1;
            }
            if signer {
                s_hash = mix64(s_hash, pos as u64);
                rec.signer_account_count += 1;
                if pos < 64 {
                    rec.signer_position_pattern |= 1u64 << pos;
                }
            }
            if idx < static_len {
                rec.static_account_ref_count += 1;
            } else {
                rec.alt_account_ref_count += 1;
            }
            if seen_here[idx] == 0 {
                seen_here[idx] = 1;
                expanded[idx].usage_count += 1;
                expanded[idx].used_as_instruction_account = 1;
                if expanded[idx].first_instruction_seen == INDEX_NONE {
                    expanded[idx].first_instruction_seen = ix_i as u16;
                }
                expanded[idx].last_instruction_seen = ix_i as u16;
            } else if seen_here[idx] == 1 {
                seen_here[idx] = 2;
                rec.reused_account_count += 1;
                if writable {
                    rec.reused_writable_account_count += 1;
                }
            }
            if writable && !this_w[idx] {
                expanded[idx].writable_usage_count += 1;
                this_w[idx] = true;
            }
        }
        if seen_here[pidx] == 0 {
            expanded[pidx].usage_count += 1;
            if expanded[pidx].first_instruction_seen == INDEX_NONE {
                expanded[pidx].first_instruction_seen = ix_i as u16;
            }
            expanded[pidx].last_instruction_seen = ix_i as u16;
        }
        expanded[pidx].program_usage_count += 1;
        expanded[pidx].used_as_program = 1;

        rec.account_count = ix.accounts.len() as u16;
        rec.position_pattern_complete = u8::from(ix.accounts.len() <= 64);
        if rec.alt_role == 0 {
            rec.alt_role = ALT_ROLE_NONE;
        }
        rec.structural_family_hash64 = structural_family(
            ix.accounts.len() as u64,
            w_hash,
            s_hash,
            rec.data_len_class as u64,
        );

        total_refs += ix.accounts.len() as u32;
        total_data += ix.data.len() as u32;
        if rec.data_len > max_data {
            max_data = rec.data_len;
        }
        if rec.data_len < min_data {
            min_data = rec.data_len;
        }
        out.instructions[ix_i] = rec;
    }

    // Adjacency is a plain set intersection over instruction account sets.
    // It does not use the fast path's bitset.
    let mut adj_any = 0u16;
    let mut adj_w = 0u16;
    let mut prev_set: Vec<bool> = vec![false; expanded.len()];
    let mut prev_ws: Vec<bool> = vec![false; expanded.len()];
    let mut started = false;
    for ix in &tx.instructions {
        let mut cur = vec![false; expanded.len()];
        let mut cur_w = vec![false; expanded.len()];
        let pidx = ix.program as usize;
        cur[pidx] = true;
        for &b in &ix.accounts {
            let idx = b as usize;
            cur[idx] = true;
            if expanded[idx].writable == 1 {
                cur_w[idx] = true;
            }
        }
        if started {
            if cur.iter().zip(prev_set.iter()).any(|(a, b)| *a && *b) {
                adj_any += 1;
            }
            if cur_w.iter().zip(prev_ws.iter()).any(|(a, b)| *a && *b) {
                adj_w += 1;
            }
        }
        prev_set = cur;
        prev_ws = cur_w;
        started = true;
    }

    for (ix_i, ix) in tx.instructions.iter().enumerate() {
        let pidx = ix.program as usize;
        let mut shared = expanded[pidx].usage_count > 1;
        let mut sw = false;
        for &b in &ix.accounts {
            let idx = b as usize;
            if expanded[idx].usage_count > 1 {
                shared = true;
            }
            if expanded[idx].writable_usage_count > 1 {
                sw = true;
            }
        }
        if shared {
            shared_ix += 1;
        }
        if sw {
            shared_w += 1;
        }
        let _ = ix_i;
    }

    if tx.instructions.is_empty() {
        min_data = 0;
        max_data = 0;
    }

    let mut max_usage = 0u16;
    let mut max_w_usage = 0u16;
    let mut multi = 0u16;
    let mut multi_w = 0u16;
    let mut unique_ix_acc = 0u16;
    let mut unique_prog = 0u16;
    let mut extra_prog = 0u16;
    for a in &expanded {
        max_usage = max_usage.max(a.usage_count);
        max_w_usage = max_w_usage.max(a.writable_usage_count);
        if a.usage_count > 1 {
            multi += 1;
        }
        if a.writable_usage_count > 1 {
            multi_w += 1;
        }
        if a.used_as_instruction_account == 1 {
            unique_ix_acc += 1;
        }
        if a.program_usage_count > 0 {
            unique_prog += 1;
        }
        if a.program_usage_count > 1 {
            extra_prog += a.program_usage_count - 1;
        }
    }

    let mut program_fp = PROGRAM_SEQUENCE_IV;
    let mut structure_fp = STRUCTURE_FP_IV;
    for rec in &out.instructions[..tx.instructions.len()] {
        program_fp = mix64(program_fp, rec.program_ref_kind as u64);
        program_fp = mix64(program_fp, rec.program_identity_hash64);
        program_fp = mix64(program_fp, rec.program_alt_identity_hash64);
        structure_fp = mix64(structure_fp, rec.structural_family_hash64);
    }
    let mut topology = TOPOLOGY_FP_IV;
    topology = mix64(topology, tx.instructions.len() as u64);
    topology = mix64(topology, expanded.len() as u64);
    for a in &expanded {
        topology = mix64(topology, a.origin as u64);
        topology = mix64(topology, a.signer as u64);
        topology = mix64(topology, a.writable as u64);
        topology = mix64(topology, a.usage_count as u64);
        topology = mix64(topology, a.writable_usage_count as u64);
        topology = mix64(topology, a.used_as_program as u64);
        topology = mix64(topology, a.used_as_instruction_account as u64);
    }
    topology = mix64(topology, adj_any as u64);
    topology = mix64(topology, adj_w as u64);

    for (i, a) in expanded.iter().enumerate() {
        out.accounts[i] = *a;
    }

    let writable_signers = required - ro_s;
    let writable_unsigned = static_len - required - ro_u;

    out.header = TxHeaderV1 {
        requested_cu_price: cb.price,
        program_sequence_fingerprint: program_fp,
        instruction_structure_fingerprint: structure_fp,
        topology_fingerprint: topology,
        schema_id: FEATURE_SCHEMA_ID,
        raw_len: raw.len() as u32,
        total_instruction_account_refs: total_refs,
        reused_account_ref_count: total_refs.saturating_sub(unique_ix_acc as u32),
        total_instruction_data_bytes: total_data,
        max_instruction_data_len: max_data,
        min_instruction_data_len: min_data,
        compute_budget_flags: cb.flags,
        requested_cu_limit: cb.limit,
        requested_heap_bytes: cb.heap,
        requested_loaded_accounts_data_size: cb.data_size,
        deprecated_requested_units: cb.deprecated_units,
        deprecated_additional_fee: cb.deprecated_fee,
        signature_count: tx.signature_count,
        required_signature_count: tx.required,
        readonly_signed_count: tx.readonly_signed,
        readonly_unsigned_count: tx.readonly_unsigned,
        static_account_count: static_len as u16,
        total_account_reference_count: expanded.len() as u16,
        static_writable_count: (writable_signers + writable_unsigned) as u16,
        static_readonly_count: (ro_s + ro_u) as u16,
        static_signer_count: tx.required,
        instruction_count: tx.instructions.len() as u16,
        unique_program_reference_count: unique_prog,
        unique_instruction_account_refs: unique_ix_acc,
        alt_lookup_count: tx.lookups.len() as u16,
        alt_loaded_writable_count: loaded_w,
        alt_loaded_readonly_count: loaded_r,
        compute_budget_instruction_count: cb.count,
        compute_budget_unknown_count: cb.unknown,
        compute_budget_malformed_count: cb.malformed,
        compute_budget_first_index: cb.first,
        compute_budget_last_index: cb.last,
        max_account_usage_count: max_usage,
        max_writable_account_usage_count: max_w_usage,
        instructions_with_shared_accounts: shared_ix,
        instructions_with_shared_writable_accounts: shared_w,
        accounts_used_by_multiple_instructions: multi,
        writable_accounts_used_by_multiple_instructions: multi_w,
        adjacent_pairs_sharing_account: adj_any,
        adjacent_pairs_sharing_writable: adj_w,
        repeated_program_invocation_count: extra_prog,
        version: tx.version,
        uses_alt: u8::from(!tx.lookups.is_empty()),
        program_repeated: u8::from(extra_prog > 0),
        _pad: [0; 7],
    };
    Ok(())
}

/// Independent v1-format walk (framed-tx-v2). Written separately from
/// `wire::parse_v1_format`; returns the owned transaction and the config as
/// `(mask, fee, cu_limit, loaded_data, heap)`.
fn parse_owned_v1_format(raw: &[u8]) -> Result<(OwnedTx, TxExtV2), Status> {
    if raw.len() > u32::MAX as usize {
        return Err(Status::CapacityExceeded);
    }
    let mut i = 0usize;
    let fixed = take(raw, &mut i, 1 + 3 + 4 + 32 + 2)?;
    if fixed[0] != 0x81 {
        return Err(Status::UnsupportedVersion);
    }
    let (required, ro_s, ro_u) = (fixed[1] as usize, fixed[2] as usize, fixed[3] as usize);
    let mask = le_u32(&fixed[4..8]);
    if mask & !0b11111 != 0 || matches!(mask & 0b11, 0b01 | 0b10) {
        return Err(Status::Malformed);
    }
    let ix_count = fixed[40] as usize;
    let key_count = fixed[41] as usize;
    if ix_count > MAX_INSTRUCTIONS || key_count > MAX_ACCOUNTS {
        return Err(Status::CapacityExceeded);
    }
    if ro_s > required || required > key_count || ro_u > key_count - required {
        return Err(Status::Malformed);
    }
    let key_base = i;
    let mut static_keys = Vec::with_capacity(key_count);
    for _ in 0..key_count {
        let mut key = [0u8; 32];
        key.copy_from_slice(take(raw, &mut i, 32)?);
        static_keys.push(key);
    }
    let mut ext = TxExtV2::zero();
    ext.config_mask = mask;
    if mask & 0b11 == 0b11 {
        ext.config_priority_fee_lamports = le_u64(take(raw, &mut i, 8)?);
    }
    if mask & 0b100 != 0 {
        ext.config_compute_unit_limit = le_u32(take(raw, &mut i, 4)?);
    }
    if mask & 0b1000 != 0 {
        ext.config_loaded_accounts_data_size = le_u32(take(raw, &mut i, 4)?);
    }
    if mask & 0b10000 != 0 {
        ext.config_heap_size = le_u32(take(raw, &mut i, 4)?);
    }
    let mut headers = Vec::with_capacity(ix_count);
    for _ in 0..ix_count {
        let h = take(raw, &mut i, 4)?;
        headers.push((h[0], h[1] as usize, le_u16(&h[2..4]) as usize));
    }
    let mut instructions = Vec::with_capacity(ix_count);
    for (program, acc_len, data_len) in headers {
        let accounts_offset = u32::try_from(i).map_err(|_| Status::CapacityExceeded)?;
        let accounts = take(raw, &mut i, acc_len)?.to_vec();
        let data_offset = u32::try_from(i).map_err(|_| Status::CapacityExceeded)?;
        let data = take(raw, &mut i, data_len)?.to_vec();
        instructions.push(OwnedIx { program, accounts, data, accounts_offset, data_offset });
    }
    take(raw, &mut i, required * 64)?;
    if i != raw.len() {
        return Err(Status::Malformed);
    }
    let tx = OwnedTx {
        version: VERSION_V1,
        signature_count: required as u16,
        required: required as u16,
        readonly_signed: ro_s as u16,
        readonly_unsigned: ro_u as u16,
        static_keys,
        key_base: key_base as u32,
        instructions,
        lookups: Vec::new(),
        runtime_alt_order: true,
    };
    Ok((tx, ext))
}

/// Independent framed-tx-v2 encoder. Allocates. For tests only.
pub fn encode_reference_v2(raw: &[u8], out: &mut EncodedTxV2) -> Status {
    let parsed = match raw.first() {
        Some(&b) if b & 0x80 == 0 => parse_owned(raw).map(|mut tx| {
            tx.runtime_alt_order = true;
            (tx, TxExtV2::zero())
        }),
        Some(&0x81) => parse_owned_v1_format(raw),
        Some(_) => Err(Status::UnsupportedVersion),
        None => Err(Status::Malformed),
    };
    match parsed.and_then(|(tx, ext)| featurize(raw, &tx, &mut out.base).map(|()| ext)) {
        Ok(ext) => {
            out.base.header.schema_id = FEATURE_SCHEMA_V2_ID;
            out.ext = ext;
            Status::Ok
        }
        Err(status) => {
            out.base.header = TxHeaderV1::zero();
            out.ext = TxExtV2::zero();
            status
        }
    }
}

/// Independent encoder. Allocates. For tests only.
pub fn encode_reference(raw: &[u8], out: &mut EncodedTxV1) -> Status {
    match parse_owned(raw).and_then(|tx| featurize(raw, &tx, out)) {
        Ok(()) => Status::Ok,
        Err(status) => {
            out.header = TxHeaderV1::zero();
            status
        }
    }
}
