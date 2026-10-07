//! Hot-path encoder. No allocation, no I/O, no locks.

use crate::accounts::rollup;
use crate::compute_budget::CbAccum;
use crate::fingerprint::{mix64, PROGRAM_SEQUENCE_IV, STRUCTURE_FP_IV, TOPOLOGY_FP_IV};
use crate::instruction::{self, derive_instruction};
use crate::limits::{FEATURE_SCHEMA_ID, FEATURE_SCHEMA_V2_ID};
use crate::status::Status;
use crate::view::{EncodedTxV1, EncodedTxV2, Scratch, TxExtV2, TxHeaderV1};
use crate::wire::{self, Frame};

/// Encode one framed Solana transaction into `out`.
///
/// `raw` is the serialized transaction (signatures and message), not JSON and
/// not a base58 signature. `scratch` and `out` are caller-owned. A successful
/// call performs no heap allocation and does not read anything except `raw`.
///
/// The same raw bytes always produce the same populated `EncodedTxV1` fields.
pub fn encode_transaction_v1(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV1) -> Status {
    match encode_inner(raw, scratch, out) {
        Ok(()) => Status::Ok,
        Err(status) => {
            out.header = TxHeaderV1::zero();
            status
        }
    }
}

/// framed-tx-v2: [`encode_transaction_v1`] plus the v1 transaction format.
///
/// For legacy and v0 transactions `out.base` is the framed-tx-v1 encoding
/// with `schema_id = 2`, except that lookup-loaded accounts follow the runtime
/// order (all tables' writable, then all readonly). That differs from v1 only
/// for transactions with two or more lookup tables. `out.ext` is zero. For the v1 format
/// (`0x81` prefix) the same features are derived from the v1 framing
/// (`version = 1`, no lookup tables) and `out.ext` carries the message config.
/// No allocation; the same raw bytes always produce the same output.
pub fn encode_transaction_v2(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV2) -> Status {
    let result = wire::parse_v2(raw, scratch, &mut out.base.accounts)
        .and_then(|frame| derive(raw, scratch, &frame, &mut out.base).map(|()| frame));
    match result {
        Ok(frame) => {
            out.base.header.schema_id = FEATURE_SCHEMA_V2_ID;
            out.ext = TxExtV2 {
                config_priority_fee_lamports: frame.config.priority_fee,
                config_mask: frame.config.mask,
                config_compute_unit_limit: frame.config.compute_unit_limit,
                config_loaded_accounts_data_size: frame.config.loaded_accounts_data_size,
                config_heap_size: frame.config.heap_size,
            };
            Status::Ok
        }
        Err(status) => {
            out.base.header = TxHeaderV1::zero();
            out.ext = TxExtV2::zero();
            status
        }
    }
}

#[cfg(feature = "phase-profile")]
pub static PHASE_CYCLES: [std::sync::atomic::AtomicU64; 4] = [
    std::sync::atomic::AtomicU64::new(0),
    std::sync::atomic::AtomicU64::new(0),
    std::sync::atomic::AtomicU64::new(0),
    std::sync::atomic::AtomicU64::new(0),
];

#[cfg(feature = "phase-profile")]
fn rdtsc() -> u64 {
    unsafe {
        core::arch::x86_64::_mm_lfence();
        core::arch::x86_64::_rdtsc()
    }
}

macro_rules! phase {
    ($slot:expr, $body:expr) => {{
        #[cfg(feature = "phase-profile")]
        let _t0 = rdtsc();
        let _v = $body;
        #[cfg(feature = "phase-profile")]
        PHASE_CYCLES[$slot].fetch_add(
            rdtsc().wrapping_sub(_t0),
            std::sync::atomic::Ordering::Relaxed,
        );
        _v
    }};
}

fn encode_inner(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV1) -> Result<(), Status> {
    let frame = phase!(0, wire::parse(raw, scratch, &mut out.accounts))?;
    derive(raw, scratch, &frame, out)
}

fn derive(
    raw: &[u8],
    scratch: &mut Scratch,
    frame: &Frame,
    out: &mut EncodedTxV1,
) -> Result<(), Status> {
    let total = frame.total_accounts as usize;
    let n_ix = frame.instruction_count as usize;
    let static_len = frame.static_accounts as usize;

    for i in 0..total {
        scratch.seen_gen[i] = 0;
        scratch.writable_gen[i] = 0;
        scratch.dup_gen[i] = 0;
    }

    let mut cb = CbAccum::new();
    let mut total_refs: u32 = 0;
    let mut total_data: u32 = 0;
    let mut max_data: u32 = 0;
    let mut min_data: u32 = u32::MAX;
    let mut adj_any: u16 = 0;
    let mut adj_w: u16 = 0;
    let mut prev_any = [0u64; 4];
    let mut prev_w = [0u64; 4];
    let mut shared_ix: u16 = 0;
    let mut shared_w_ix: u16 = 0;

    phase!(1, {
        for i in 0..n_ix {
            let gen = (i as u32).wrapping_add(1);
            let (ix, acc) = derive_instruction(
                raw,
                scratch.ixs[i],
                i as u16,
                gen,
                static_len,
                total,
                &mut out.accounts,
                &mut scratch.seen_gen,
                &mut scratch.writable_gen,
                &mut scratch.dup_gen,
                &mut cb,
            )?;
            total_refs = total_refs
                .checked_add(acc.total_refs_delta)
                .ok_or(Status::CapacityExceeded)?;
            total_data = total_data
                .checked_add(acc.data_len)
                .ok_or(Status::CapacityExceeded)?;
            if acc.data_len > max_data {
                max_data = acc.data_len;
            }
            if acc.data_len < min_data {
                min_data = acc.data_len;
            }
            if i > 0 {
                if instruction::intersects(&acc.any_bits, &prev_any) {
                    adj_any = adj_any.saturating_add(1);
                }
                if instruction::intersects(&acc.writable_bits, &prev_w) {
                    adj_w = adj_w.saturating_add(1);
                }
            }
            prev_any = acc.any_bits;
            prev_w = acc.writable_bits;
            out.instructions[i] = ix;
        }
    });

    if n_ix == 0 {
        min_data = 0;
        max_data = 0;
    }

    // Second scan: "does this instruction touch an account that another
    // instruction also touches?" Usage counts are known only after pass 1.
    phase!(2, {
        for i in 0..n_ix {
            let meta = scratch.ixs[i];
            let pidx = meta.program_index as usize;
            let mut shared = out.accounts[pidx].usage_count > 1;
            let mut shared_w = false;
            let start = meta.accounts_offset as usize;
            let len = meta.accounts_len as usize;
            let end = start.checked_add(len).ok_or(Status::Malformed)?;
            let bytes = raw.get(start..end).ok_or(Status::Malformed)?;
            for &b in bytes {
                let idx = b as usize;
                if idx >= total {
                    return Err(Status::Malformed);
                }
                if out.accounts[idx].usage_count > 1 {
                    shared = true;
                }
                if out.accounts[idx].writable_usage_count > 1 {
                    shared_w = true;
                }
            }
            if shared {
                shared_ix = shared_ix.saturating_add(1);
            }
            if shared_w {
                shared_w_ix = shared_w_ix.saturating_add(1);
            }
        }
    });

    phase!(3, {
        let usage = rollup(&out.accounts[..total]);
        let reused_refs = total_refs.saturating_sub(usage.unique_instruction_accounts as u32);

        let mut program_fp = PROGRAM_SEQUENCE_IV;
        let mut structure_fp = STRUCTURE_FP_IV;
        for ix in &out.instructions[..n_ix] {
            program_fp = mix64(program_fp, ix.program_ref_kind as u64);
            program_fp = mix64(program_fp, ix.program_identity_hash64);
            program_fp = mix64(program_fp, ix.program_alt_identity_hash64);
            structure_fp = mix64(structure_fp, ix.structural_family_hash64);
        }

        let mut topology = TOPOLOGY_FP_IV;
        topology = mix64(topology, n_ix as u64);
        topology = mix64(topology, total as u64);
        for a in &out.accounts[..total] {
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

        out.header = TxHeaderV1 {
            requested_cu_price: cb.price,
            program_sequence_fingerprint: program_fp,
            instruction_structure_fingerprint: structure_fp,
            topology_fingerprint: topology,
            schema_id: FEATURE_SCHEMA_ID,
            raw_len: raw.len() as u32,
            total_instruction_account_refs: total_refs,
            reused_account_ref_count: reused_refs,
            total_instruction_data_bytes: total_data,
            max_instruction_data_len: max_data,
            min_instruction_data_len: min_data,
            compute_budget_flags: cb.flags,
            requested_cu_limit: cb.limit,
            requested_heap_bytes: cb.heap,
            requested_loaded_accounts_data_size: cb.data_size,
            deprecated_requested_units: cb.deprecated_units,
            deprecated_additional_fee: cb.deprecated_fee,
            signature_count: frame.signature_count,
            required_signature_count: frame.required_signatures,
            readonly_signed_count: frame.readonly_signed,
            readonly_unsigned_count: frame.readonly_unsigned,
            static_account_count: frame.static_accounts,
            total_account_reference_count: frame.total_accounts,
            static_writable_count: frame.static_writable,
            static_readonly_count: frame.static_readonly,
            static_signer_count: frame.required_signatures,
            instruction_count: frame.instruction_count,
            unique_program_reference_count: usage.unique_programs,
            unique_instruction_account_refs: usage.unique_instruction_accounts,
            alt_lookup_count: frame.alt_lookup_count,
            alt_loaded_writable_count: frame.alt_loaded_writable,
            alt_loaded_readonly_count: frame.alt_loaded_readonly,
            compute_budget_instruction_count: cb.count,
            compute_budget_unknown_count: cb.unknown,
            compute_budget_malformed_count: cb.malformed,
            compute_budget_first_index: cb.first,
            compute_budget_last_index: cb.last,
            max_account_usage_count: usage.max_usage,
            max_writable_account_usage_count: usage.max_writable_usage,
            instructions_with_shared_accounts: shared_ix,
            instructions_with_shared_writable_accounts: shared_w_ix,
            accounts_used_by_multiple_instructions: usage.accounts_used_by_multiple,
            writable_accounts_used_by_multiple_instructions: usage
                .writable_accounts_used_by_multiple,
            adjacent_pairs_sharing_account: adj_any,
            adjacent_pairs_sharing_writable: adj_w,
            repeated_program_invocation_count: usage.repeated_program_invocations,
            version: frame.version,
            uses_alt: u8::from(frame.alt_lookup_count > 0),
            program_repeated: u8::from(usage.repeated_program_invocations > 0),
            _pad: [0; 7],
        };
    });
    Ok(())
}
