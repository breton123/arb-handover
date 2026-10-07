use crate::compute_budget::{
    CB_DATA_SIZE, CB_DEPRECATED, CB_DUP_LIMIT, CB_HEAP, CB_LIMIT, CB_MALFORMED_PAYLOAD, CB_PRICE,
    CB_UNKNOWN, COMPUTE_BUDGET_PROGRAM_ID,
};
use crate::fingerprint::identity_hash64;
use crate::limits::{
    FEATURE_SCHEMA_ID, INDEX_NONE, ORIGIN_ALT_WRITABLE, ORIGIN_STATIC, PROGRAM_ALT_REF, PROGRAM_STATIC,
    VERSION_LEGACY, VERSION_V0,
};
use crate::reference::encode_reference;
use crate::view::{copied_input_bytes, write_canonical, AccountRoleV1, EncodedTxV1, InstructionFeaturesV1, Scratch, TxHeaderV1};
use crate::{encode_transaction_v1, Status, MAX_INSTRUCTIONS};
pub(crate) fn push_short(buf: &mut Vec<u8>, mut len: usize) {
    loop {
        let mut elem = (len & 0x7f) as u8;
        len >>= 7;
        if len == 0 {
            buf.push(elem);
            break;
        }
        elem |= 0x80;
        buf.push(elem);
    }
}

#[derive(Clone)]
pub(crate) struct BuiltIx {
    pub(crate) program: u8,
    pub(crate) accounts: Vec<u8>,
    pub(crate) data: Vec<u8>,
}

#[derive(Clone)]
pub(crate) struct BuiltLookup {
    pub(crate) key: [u8; 32],
    pub(crate) writable: Vec<u8>,
    pub(crate) readonly: Vec<u8>,
}

pub(crate) fn key_byte(b: u8) -> [u8; 32] {
    let mut k = [0u8; 32];
    k[0] = b;
    k[31] = b.wrapping_mul(3);
    k
}

pub(crate) fn build(
    v0: bool,
    sigs: usize,
    required: u8,
    ro_s: u8,
    ro_u: u8,
    keys: &[[u8; 32]],
    ixs: &[BuiltIx],
    lookups: &[BuiltLookup],
) -> Vec<u8> {
    let mut buf = Vec::new();
    push_short(&mut buf, sigs);
    for s in 0..sigs {
        buf.extend(std::iter::repeat(0x11 + s as u8).take(64));
    }
    if v0 {
        buf.push(0x80);
    }
    buf.push(required);
    buf.push(ro_s);
    buf.push(ro_u);
    push_short(&mut buf, keys.len());
    for k in keys {
        buf.extend_from_slice(k);
    }
    buf.extend_from_slice(&[0xAB; 32]);
    push_short(&mut buf, ixs.len());
    for ix in ixs {
        buf.push(ix.program);
        push_short(&mut buf, ix.accounts.len());
        buf.extend_from_slice(&ix.accounts);
        push_short(&mut buf, ix.data.len());
        buf.extend_from_slice(&ix.data);
    }
    if v0 {
        push_short(&mut buf, lookups.len());
        for l in lookups {
            buf.extend_from_slice(&l.key);
            push_short(&mut buf, l.writable.len());
            buf.extend_from_slice(&l.writable);
            push_short(&mut buf, l.readonly.len());
            buf.extend_from_slice(&l.readonly);
        }
    }
    buf
}

fn run(raw: &[u8]) -> (Status, Box<EncodedTxV1>, Box<EncodedTxV1>) {
    let mut scratch = Scratch::new();
    let mut fast = EncodedTxV1::boxed();
    let st = encode_transaction_v1(raw, &mut scratch, &mut fast);
    let mut slow = EncodedTxV1::boxed();
    let st2 = encode_reference(raw, &mut slow);
    assert_eq!(st, st2, "fast/reference status diverge on {} bytes", raw.len());
    if st == Status::Ok {
        assert_eq!(fast.header, slow.header);
        let n = fast.header.instruction_count as usize;
        let m = fast.header.total_account_reference_count as usize;
        assert_eq!(&fast.instructions[..n], &slow.instructions[..n]);
        assert_eq!(&fast.accounts[..m], &slow.accounts[..m]);
        assert_invariants(&fast, raw);
    } else {
        assert_eq!(fast.header, TxHeaderV1::zero());
        assert_eq!(slow.header, TxHeaderV1::zero());
    }
    (st, fast, slow)
}

fn assert_invariants(out: &EncodedTxV1, raw: &[u8]) {
    assert_invariants_with(out, raw, FEATURE_SCHEMA_ID);
}

pub(crate) fn assert_invariants_with(out: &EncodedTxV1, raw: &[u8], schema_id: u32) {
    let h = &out.header;
    assert_eq!(h.schema_id, schema_id);
    assert_eq!(h.raw_len as usize, raw.len());
    assert_eq!(
        h.static_writable_count + h.static_readonly_count,
        h.static_account_count
    );
    assert!(h.instruction_count as usize <= MAX_INSTRUCTIONS);
    assert!(h.total_account_reference_count as usize <= 256);
    let n = h.instruction_count as usize;
    let m = h.total_account_reference_count as usize;
    let mut refs = 0u32;
    let mut data = 0u32;
    let mut max_d = 0u32;
    let mut min_d = u32::MAX;
    for ix in &out.instructions[..n] {
        assert_eq!(
            ix.writable_account_count + ix.readonly_account_count,
            ix.account_count
        );
        assert_eq!(
            ix.static_account_ref_count + ix.alt_account_ref_count,
            ix.account_count
        );
        let ds = ix.data_offset as usize;
        let de = ds + ix.data_len as usize;
        assert!(de <= raw.len());
        let data_bytes = &raw[ds..de];
        let head_n = ix.head_len as usize;
        assert_eq!(head_n, data_bytes.len().min(32));
        assert_eq!(&ix.head[..head_n], &data_bytes[..head_n]);
        assert_eq!(&ix.head[head_n..], &[0u8; 32][..32 - head_n]);
        if ix.prefix_flags & 1 != 0 {
            assert_eq!(ix.prefix1, data_bytes[0]);
        }
        let as_ = ix.accounts_offset as usize;
        let ae = as_ + ix.accounts_len as usize;
        assert!(ae <= raw.len());
        assert_eq!(ae - as_, ix.account_count as usize);
        for &b in &raw[as_..ae] {
            assert!((b as usize) < m);
        }
        assert!((ix.program_account_index as usize) < m);
        if ix.program_ref_kind == PROGRAM_STATIC {
            let off = ix.program_key_offset as usize;
            assert!(off + 32 <= raw.len());
            assert_eq!(ix.program_identity_available, 1);
            assert_eq!(ix.program_identity_hash64, identity_hash64(&raw[off..off + 32]));
            assert_eq!(ix.program_alt_identity_hash64, 0);
        }
        if ix.program_ref_kind == PROGRAM_ALT_REF {
            assert_eq!(ix.program_identity_available, 0);
            assert_eq!(ix.program_identity_hash64, 0);
            assert_ne!(ix.alt_table_ordinal, INDEX_NONE);
            let off = ix.program_key_offset as usize;
            assert!(off + 32 <= raw.len());
        }
        refs += ix.account_count as u32;
        data += ix.data_len;
        max_d = max_d.max(ix.data_len);
        min_d = min_d.min(ix.data_len);
    }
    if n == 0 {
        min_d = 0;
    }
    assert_eq!(refs, h.total_instruction_account_refs);
    assert_eq!(data, h.total_instruction_data_bytes);
    assert_eq!(max_d, h.max_instruction_data_len);
    assert_eq!(min_d, h.min_instruction_data_len);
    for a in &out.accounts[..m] {
        if a.origin == ORIGIN_STATIC {
            let off = a.static_pubkey_offset as usize;
            assert_eq!(a.static_pubkey_len, 32);
            assert!(off + 32 <= raw.len());
            assert_eq!(a.identity_hash64, identity_hash64(&raw[off..off + 32]));
            assert_eq!(a.has_static_identity, 1);
            assert_eq!(a.alt_identity_hash64, 0);
        } else {
            assert_eq!(a.has_static_identity, 0);
            assert_eq!(a.identity_hash64, 0);
            assert_eq!(a.signer, 0);
            assert_eq!(a.fee_payer, 0);
            assert_eq!(a.alt_table_pubkey_len, 32);
            let off = a.alt_table_pubkey_offset as usize;
            assert!(off + 32 <= raw.len());
        }
    }
    let _ = copied_input_bytes(out);
}

#[test]
fn layout_matches_v1_abi() {
    assert_eq!(std::mem::size_of::<TxHeaderV1>(), 152);
    assert_eq!(std::mem::size_of::<InstructionFeaturesV1>(), 152);
    assert_eq!(std::mem::size_of::<AccountRoleV1>(), 56);
    assert_eq!(
        std::mem::size_of::<EncodedTxV1>(),
        152 + 152 * MAX_INSTRUCTIONS + 56 * 256
    );
    assert_eq!(std::mem::size_of::<Scratch>(), 18432);
    assert_eq!(std::mem::align_of::<EncodedTxV1>(), 8);
    assert_eq!(
        std::mem::offset_of!(TxHeaderV1, schema_id),
        32
    );
    assert_eq!(std::mem::offset_of!(TxHeaderV1, version), 142);
    assert_eq!(std::mem::offset_of!(EncodedTxV1, instructions), 152);
}

#[test]
fn identity_hash_matches_frozen_vectors() {
    assert_eq!(identity_hash64(b""), 0x243f6a8885a308d3);
    assert_eq!(identity_hash64(&[0x01]), 0xfc697677af4d17e4);
    assert_eq!(identity_hash64(&[0u8; 32]), 0x69fd5e9a82e62945);
    assert_eq!(
        identity_hash64(&COMPUTE_BUDGET_PROGRAM_ID),
        0x818e1c968224f269
    );
}

#[test]
fn legacy_and_v0_real_shapes() {
    let payer = key_byte(1);
    let program = key_byte(2);
    let raw = build(
        false,
        1,
        1,
        0,
        1,
        &[payer, program],
        &[
            BuiltIx {
                program: 1,
                accounts: vec![],
                data: vec![0x02, 0x10, 0x00, 0x00, 0x00],
            },
            BuiltIx {
                program: 1,
                accounts: vec![0],
                data: vec![0x99],
            },
        ],
        &[],
    );
    let (st, out, _) = run(&raw);
    assert_eq!(st, Status::Ok);
    assert_eq!(out.header.version, VERSION_LEGACY);
    assert_eq!(out.header.signature_count, 1);
    assert_eq!(out.header.static_account_count, 2);
    assert_eq!(out.header.uses_alt, 0);
    assert_eq!(out.header.instruction_count, 2);
    assert_eq!(out.instructions[1].head_len, 1);
    assert_eq!(out.instructions[1].prefix_flags, 1);
    assert_eq!(out.instructions[1].prefix1, 0x99);
    assert_eq!(out.instructions[1].data_len_class, 1);

    let mut lookups = vec![BuiltLookup {
        key: key_byte(9),
        writable: vec![4, 7],
        readonly: vec![1],
    }];
    let raw_v0 = build(
        true,
        1,
        1,
        0,
        0,
        &[payer, program],
        &[BuiltIx {
            program: 1,
            accounts: vec![0, 2, 4],
            data: vec![1, 2, 3, 4, 5, 6, 7, 8, 9],
        }],
        &lookups,
    );
    let (st, out, _) = run(&raw_v0);
    assert_eq!(st, Status::Ok);
    assert_eq!(out.header.version, VERSION_V0);
    assert_eq!(out.header.uses_alt, 1);
    assert_eq!(out.header.alt_lookup_count, 1);
    assert_eq!(out.header.alt_loaded_writable_count, 2);
    assert_eq!(out.header.alt_loaded_readonly_count, 1);
    assert_eq!(out.header.total_account_reference_count, 5);
    assert_eq!(out.accounts[2].origin, ORIGIN_ALT_WRITABLE);
    assert_eq!(out.accounts[2].alt_address_index, 4);
    assert_eq!(out.accounts[2].has_static_identity, 0);
    assert_eq!(out.instructions[0].alt_account_ref_count, 2);
    assert_eq!(out.instructions[0].static_account_ref_count, 1);
    assert_eq!(out.instructions[0].prefix_flags, 0b01111); // 1,2,4,8
    lookups.clear();
}

#[test]
fn multiple_signatures_and_roles() {
    let keys = [key_byte(1), key_byte(2), key_byte(3)];
    let raw = build(
        false,
        2,
        2,
        1,
        1,
        &keys,
        &[BuiltIx {
            program: 2,
            accounts: vec![0, 1],
            data: vec![],
        }],
        &[],
    );
    let (st, out, _) = run(&raw);
    assert_eq!(st, Status::Ok);
    assert_eq!(out.header.signature_count, 2);
    assert_eq!(out.header.static_signer_count, 2);
    assert_eq!(out.header.readonly_signed_count, 1);
    assert_eq!(out.header.static_writable_count, 1);
    assert_eq!(out.header.static_readonly_count, 2);
    assert_eq!(out.accounts[0].fee_payer, 1);
    assert_eq!(out.accounts[0].writable, 1);
    assert_eq!(out.accounts[0].signer, 1);
    assert_eq!(out.accounts[1].signer, 1);
    assert_eq!(out.accounts[1].writable, 0);
    assert_eq!(out.accounts[2].signer, 0);
    assert_eq!(out.accounts[2].readonly, 1);
    assert_eq!(out.instructions[0].data_len, 0);
    assert_eq!(out.instructions[0].head_len, 0);
    assert_eq!(out.instructions[0].prefix_flags, 0);
    assert_eq!(out.instructions[0].signer_account_count, 2);
}

#[test]
fn repeated_accounts_and_programs() {
    let keys = [key_byte(1), key_byte(2), key_byte(3)];
    let raw = build(
        false,
        1,
        1,
        0,
        2,
        &keys,
        &[
            BuiltIx {
                program: 1,
                accounts: vec![0, 0, 2],
                data: vec![1, 2],
            },
            BuiltIx {
                program: 1,
                accounts: vec![0, 2],
                data: vec![9],
            },
        ],
        &[],
    );
    let (st, out, _) = run(&raw);
    assert_eq!(st, Status::Ok);
    assert_eq!(out.instructions[0].reused_account_count, 1);
    assert_eq!(out.instructions[0].reused_writable_account_count, 1);
    assert_eq!(out.header.total_instruction_account_refs, 5);
    assert_eq!(out.header.unique_instruction_account_refs, 2);
    assert_eq!(out.header.reused_account_ref_count, 3);
    assert_eq!(out.header.program_repeated, 1);
    assert_eq!(out.header.repeated_program_invocation_count, 1);
    assert_eq!(out.header.unique_program_reference_count, 1);
    assert_eq!(out.accounts[0].usage_count, 2);
    assert_eq!(out.accounts[0].writable_usage_count, 2);
    assert_eq!(out.accounts[1].program_usage_count, 2);
    assert_eq!(out.header.max_writable_account_usage_count, 2);
    assert_eq!(out.header.instructions_with_shared_writable_accounts, 2);
    assert_eq!(out.header.adjacent_pairs_sharing_account, 1);
    assert_eq!(out.header.adjacent_pairs_sharing_writable, 1);
    assert_eq!(out.header.accounts_used_by_multiple_instructions, 3);
}

#[test]
fn unresolved_alt_program_reference() {
    let payer = key_byte(1);
    let table = key_byte(0x77);
    let raw = build(
        true,
        1,
        1,
        0,
        0,
        &[payer],
        &[BuiltIx {
            program: 1,
            accounts: vec![0],
            data: vec![0xAB, 0xCD, 0xEF, 0x01],
        }],
        &[BuiltLookup {
            key: table,
            writable: vec![3],
            readonly: vec![],
        }],
    );
    let (st, out, _) = run(&raw);
    assert_eq!(st, Status::Ok);
    let ix = &out.instructions[0];
    assert_eq!(ix.program_ref_kind, PROGRAM_ALT_REF);
    assert_eq!(ix.program_identity_available, 0);
    assert_eq!(ix.program_identity_hash64, 0);
    assert_eq!(ix.alt_table_ordinal, 0);
    assert_eq!(ix.alt_address_index, 3);
    assert_eq!(ix.alt_role, 1);
    let off = ix.program_key_offset as usize;
    assert_eq!(&raw[off..off + 32], &table);
    assert_eq!(out.header.compute_budget_instruction_count, 0);
    assert_ne!(ix.program_alt_identity_hash64, 0);
}

#[test]
fn compute_budget_exact_layouts() {
    let payer = key_byte(1);
    let mut limit = vec![2, 0x10, 0x00, 0x00, 0x00];
    let price = vec![3, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
    let heap = vec![1, 0x00, 0x00, 0x04, 0x00];
    let data_size = vec![4, 0x80, 0x96, 0x98, 0x00];
    let deprecated = vec![0, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00];
    let bad = vec![2, 0x01, 0x02, 0x03, 0x04, 0x05];
    let unknown = vec![9, 0x00];
    let second_limit = vec![2, 0x20, 0x00, 0x00, 0x00];
    let ixs = vec![
        BuiltIx { program: 1, accounts: vec![], data: limit.clone() },
        BuiltIx { program: 1, accounts: vec![], data: price },
        BuiltIx { program: 1, accounts: vec![], data: heap },
        BuiltIx { program: 1, accounts: vec![], data: data_size },
        BuiltIx { program: 1, accounts: vec![], data: deprecated },
        BuiltIx { program: 1, accounts: vec![], data: bad },
        BuiltIx { program: 1, accounts: vec![], data: unknown },
        BuiltIx { program: 1, accounts: vec![], data: second_limit },
    ];
    let raw = build(false, 1, 1, 0, 1, &[payer, COMPUTE_BUDGET_PROGRAM_ID], &ixs, &[]);
    let (st, out, _) = run(&raw);
    assert_eq!(st, Status::Ok);
    let h = &out.header;
    assert_eq!(h.compute_budget_instruction_count, 8);
    assert_eq!(h.requested_cu_limit, 0x20);
    assert_eq!(h.requested_cu_price, 100);
    assert_eq!(h.requested_heap_bytes, 0x00040000);
    assert_eq!(h.requested_loaded_accounts_data_size, 10_000_000);
    assert_eq!(h.deprecated_requested_units, 1);
    assert_eq!(h.deprecated_additional_fee, 2);
    assert_eq!(h.compute_budget_unknown_count, 1);
    assert_eq!(h.compute_budget_malformed_count, 1);
    assert_eq!(h.compute_budget_first_index, 0);
    assert_eq!(h.compute_budget_last_index, 7);
    assert_ne!(h.compute_budget_flags & CB_LIMIT, 0);
    assert_ne!(h.compute_budget_flags & CB_PRICE, 0);
    assert_ne!(h.compute_budget_flags & CB_HEAP, 0);
    assert_ne!(h.compute_budget_flags & CB_DATA_SIZE, 0);
    assert_ne!(h.compute_budget_flags & CB_DEPRECATED, 0);
    assert_ne!(h.compute_budget_flags & CB_DUP_LIMIT, 0);
    assert_ne!(h.compute_budget_flags & CB_MALFORMED_PAYLOAD, 0);
    assert_ne!(h.compute_budget_flags & CB_UNKNOWN, 0);
    // The first limit must not survive as the recorded value.
    assert_ne!(h.requested_cu_limit, 0x10);
    limit.clear();
}

#[test]
fn malformed_truncated_and_capacity() {
    assert_eq!(run(&[]).0, Status::Malformed);
    assert_eq!(run(&[0x80]).0, Status::Malformed);
    assert_eq!(run(&[0x80, 0x80, 0x80]).0, Status::Malformed);
    assert_eq!(run(&[0x80, 0x80, 0x04]).0, Status::Malformed);
    assert_eq!(run(&[0x00, 0x81]).0, Status::UnsupportedVersion);

    // One signature declared, no signature bytes.
    assert_eq!(run(&[0x01]).0, Status::Malformed);

    // One account declared, header consistent, keys truncated.
    let mut trunc_keys = vec![0x00, 0x00, 0x00, 0x00, 0x01];
    trunc_keys.extend_from_slice(&[0u8; 10]);
    assert_eq!(run(&trunc_keys).0, Status::Malformed);

    // 257 static keys: capacity as soon as the count is known.
    let mut too_many_keys = vec![0x00, 0x00, 0x00, 0x00];
    push_short(&mut too_many_keys, 257);
    assert_eq!(run(&too_many_keys).0, Status::CapacityExceeded);

    // Instruction program index out of range.
    let raw = build(
        false,
        1,
        1,
        0,
        0,
        &[key_byte(1)],
        &[BuiltIx {
            program: 5,
            accounts: vec![],
            data: vec![1],
        }],
        &[],
    );
    assert_eq!(run(&raw).0, Status::Malformed);

    // Account index out of range.
    let raw = build(
        false,
        1,
        1,
        0,
        1,
        &[key_byte(1), key_byte(2)],
        &[BuiltIx {
            program: 1,
            accounts: vec![9],
            data: vec![],
        }],
        &[],
    );
    assert_eq!(run(&raw).0, Status::Malformed);

    // readonly signed > required.
    let mut bad_header = Vec::new();
    push_short(&mut bad_header, 1);
    bad_header.extend_from_slice(&[0u8; 64]);
    bad_header.extend_from_slice(&[1, 2, 0]);
    assert_eq!(run(&bad_header).0, Status::Malformed);

    // signature count != required signatures.
    let mut mismatch = Vec::new();
    push_short(&mut mismatch, 1);
    mismatch.extend_from_slice(&[0u8; 64]);
    mismatch.push(2); // legacy required = 2
    mismatch.push(0);
    mismatch.push(0);
    assert_eq!(run(&mismatch).0, Status::Malformed);

    // Trailing byte on a valid transaction.
    let mut ok = build(false, 1, 1, 0, 0, &[key_byte(1)], &[], &[]);
    assert_eq!(run(&ok).0, Status::Ok);
    ok.push(0);
    assert_eq!(run(&ok).0, Status::Malformed);

    // More instructions than the fixed table.
    let ix = BuiltIx {
        program: 0,
        accounts: vec![],
        data: vec![],
    };
    let many = vec![ix; MAX_INSTRUCTIONS + 1];
    // program 0 with 1 account (the fee payer) and required 1.
    let raw = build(false, 1, 1, 0, 0, &[key_byte(1)], &many, &[]);
    assert_eq!(run(&raw).0, Status::CapacityExceeded);

    // 65 lookup tables.
    let lookups = vec![
        BuiltLookup {
            key: key_byte(3),
            writable: vec![],
            readonly: vec![],
        };
        65
    ];
    let raw = build(true, 1, 1, 0, 0, &[key_byte(1)], &[], &lookups);
    assert_eq!(run(&raw).0, Status::CapacityExceeded);
}

#[test]
fn deterministic_and_dirty_buffers() {
    let raw = build(
        false,
        1,
        1,
        0,
        1,
        &[key_byte(4), key_byte(5)],
        &[BuiltIx {
            program: 1,
            accounts: vec![0, 0],
            data: vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17],
        }],
        &[],
    );
    let mut scratch = Scratch::new();
    let mut a = EncodedTxV1::boxed();
    let mut b = EncodedTxV1::boxed();
    unsafe {
        std::ptr::write_bytes(a.as_mut(), 0xFF, 1);
        std::ptr::write_bytes(b.as_mut(), 0xA5, 1);
        // Dirty the scratch too.
        std::ptr::write_bytes(&mut scratch, 0x3C, 1);
    }
    assert_eq!(encode_transaction_v1(&raw, &mut scratch, &mut a), Status::Ok);
    assert_eq!(encode_transaction_v1(&raw, &mut scratch, &mut b), Status::Ok);
    let mut ba = vec![0u8; 1 << 20];
    let mut bb = vec![0u8; 1 << 20];
    let na = write_canonical(&a, &mut ba).unwrap();
    let nb = write_canonical(&b, &mut bb).unwrap();
    assert_eq!(na, nb);
    assert_eq!(&ba[..na], &bb[..nb]);
    assert_eq!(a.header, b.header);
    assert_eq!(a.instructions[0].prefix_flags & 0b11111, 0b11111);
    assert_eq!(a.instructions[0].data_len_class, 5);
}

#[test]
fn pilot_jsonl_matches_reference_and_known_fields() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../raw-tx-snippet.jsonl");
    let text = std::fs::read_to_string(path).expect("pilot jsonl");
    let mut n = 0;
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let idx = json_u64(line, "tx_index");
        let raw = decode_hex(&json_str(line, "raw_hex"));
        assert_eq!(raw.len() as u64, json_u64(line, "raw_len"));
        let (st, out, _) = run(&raw);
        assert_eq!(st, Status::Ok, "tx {idx}");
        match idx {
            0 => {
                assert_eq!(out.header.version, VERSION_LEGACY);
                assert_eq!(out.header.instruction_count, 4);
                assert_eq!(out.header.static_account_count, 19);
                assert_eq!(out.header.requested_cu_limit, 83_000);
                assert_ne!(out.header.compute_budget_flags & CB_LIMIT, 0);
                assert_eq!(out.header.compute_budget_flags & CB_PRICE, 0);
            }
            1 => {
                assert_eq!(out.header.version, VERSION_V0);
                assert_eq!(out.header.uses_alt, 1);
                assert_eq!(out.header.alt_lookup_count, 2);
                assert_eq!(out.header.alt_loaded_writable_count, 15);
                assert_eq!(out.header.alt_loaded_readonly_count, 15);
                assert_eq!(out.header.static_account_count, 16);
                assert_eq!(out.header.total_account_reference_count, 46);
                assert_eq!(out.header.instruction_count, 8);
                assert_eq!(out.header.requested_cu_limit, 520_000);
                assert_eq!(out.header.requested_cu_price, 576_923);
            }
            2 => {
                assert_eq!(out.header.signature_count, 2);
                assert_eq!(out.header.readonly_signed_count, 1);
                assert_eq!(out.header.alt_loaded_writable_count, 4);
                assert_eq!(out.header.alt_loaded_readonly_count, 8);
            }
            3 => {
                assert_eq!(out.header.version, VERSION_LEGACY);
                assert_eq!(out.header.raw_len, 250);
                assert_eq!(out.header.instruction_count, 2);
                assert_eq!(out.header.compute_budget_instruction_count, 2);
                assert_eq!(out.header.compute_budget_malformed_count, 1);
                assert_eq!(out.header.requested_cu_price, 4_166_666);
                assert_eq!(out.header.compute_budget_flags & CB_LIMIT, 0);
                assert_eq!(out.header.total_instruction_data_bytes, 78);
                assert_eq!(out.header.min_instruction_data_len, 9);
                assert_eq!(out.header.max_instruction_data_len, 69);
                assert_eq!(out.accounts[0].origin, ORIGIN_STATIC);
                assert_eq!(out.accounts[0].fee_payer, 1);
                assert_eq!(out.accounts[1].program_usage_count, 2);
            }
            4 => {
                assert_eq!(out.header.requested_cu_limit, 3_100);
                assert_eq!(out.header.requested_loaded_accounts_data_size, 10_000_000);
            }
            5 => {
                assert_eq!(out.header.compute_budget_instruction_count, 3);
                assert_eq!(out.header.compute_budget_malformed_count, 1);
                assert_eq!(out.header.requested_cu_price, 300_000);
                assert_eq!(out.header.compute_budget_flags & CB_LIMIT, 0);
                assert_eq!(out.header.requested_loaded_accounts_data_size, 273_464);
                assert_eq!(out.header.alt_loaded_writable_count, 14);
                assert_eq!(out.header.alt_loaded_readonly_count, 2);
            }
            6 => {
                assert_eq!(out.header.instruction_count, 8);
                assert_eq!(out.header.uses_alt, 1);
            }
            7 => {
                assert_eq!(out.header.version, VERSION_LEGACY);
                assert_eq!(out.header.static_account_count, 27);
                assert_eq!(out.header.instruction_count, 4);
                assert_eq!(out.header.requested_cu_limit, 300_000);
            }
            _ => panic!("unexpected tx {idx}"),
        }
        n += 1;
    }
    assert_eq!(n, 8);
}

#[test]
fn fuzz_never_panics_and_is_deterministic() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../raw-tx-snippet.jsonl");
    let text = std::fs::read_to_string(path).unwrap();
    let mut seeds: Vec<Vec<u8>> = text
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| decode_hex(&json_str(l, "raw_hex")))
        .collect();
    let mut rng = Lcg::new(0xC0FFEE);
    for _ in 0..64 {
        let base = seeds[rng.next() as usize % seeds.len()].clone();
        let mut flipped = base;
        if !flipped.is_empty() {
            let i = rng.next() as usize % flipped.len();
            flipped[i] ^= 1 << (rng.next() % 8);
        }
        seeds.push(flipped);
    }
    let mut scratch = Scratch::new();
    let mut out_a = EncodedTxV1::boxed();
    let mut out_b = EncodedTxV1::boxed();
    let mut buf = vec![0u8; 2048];
    for sample in seeds.iter() {
        check_twice(sample, &mut scratch, &mut out_a, &mut out_b);
    }
    for _ in 0..1500 {
        let n = (rng.next() as usize) % buf.len();
        for b in &mut buf[..n] {
            *b = (rng.next() & 0xff) as u8;
        }
        check_twice(&buf[..n], &mut scratch, &mut out_a, &mut out_b);
    }
}

fn check_twice(raw: &[u8], scratch: &mut Scratch, a: &mut EncodedTxV1, b: &mut EncodedTxV1) {
    let s1 = encode_transaction_v1(raw, scratch, a);
    let mut c1 = [0u8; 4096];
    let n1 = if s1 == Status::Ok {
        // Canonical image can exceed 4096 for huge successes; those are checked
        // by a second encode of the header and populated prefix via PartialEq.
        write_canonical(a, &mut c1)
    } else {
        assert_eq!(a.header, TxHeaderV1::zero());
        None
    };
    let header = a.header;
    let n_ix = header.instruction_count;
    let n_acc = header.total_account_reference_count;
    let s2 = encode_transaction_v1(raw, scratch, b);
    assert_eq!(s1, s2);
    if s1 == Status::Ok {
        assert_eq!(header, b.header);
        assert_eq!(
            &a.instructions[..n_ix as usize],
            &b.instructions[..n_ix as usize]
        );
        assert_eq!(&a.accounts[..n_acc as usize], &b.accounts[..n_acc as usize]);
        if let Some(n) = n1 {
            let mut c2 = [0u8; 4096];
            let n2 = write_canonical(b, &mut c2).unwrap();
            assert_eq!(n, n2);
            assert_eq!(&c1[..n], &c2[..n]);
        }
        let m = n_acc as usize;
        let ni = n_ix as usize;
        for ix in &b.instructions[..ni] {
            assert!(ix.data_offset as usize + ix.data_len as usize <= raw.len());
            assert!(ix.accounts_offset as usize + ix.accounts_len as usize <= raw.len());
        }
        for acct in &b.accounts[..m] {
            if acct.static_pubkey_len == 32 {
                assert!(acct.static_pubkey_offset as usize + 32 <= raw.len());
            }
            if acct.alt_table_pubkey_len == 32 {
                assert!(acct.alt_table_pubkey_offset as usize + 32 <= raw.len());
            }
        }
    }
}

#[test]
fn ffi_matches_rust_and_rejects_null() {
    let raw = build(
        false,
        1,
        1,
        0,
        0,
        &[key_byte(1), COMPUTE_BUDGET_PROGRAM_ID],
        &[BuiltIx {
            program: 1,
            accounts: vec![],
            data: vec![2, 0xE8, 0x03, 0x00, 0x00],
        }],
        &[],
    );
    let mut scratch = Scratch::new();
    let mut via_rust = EncodedTxV1::boxed();
    assert_eq!(
        encode_transaction_v1(&raw, &mut scratch, &mut via_rust),
        Status::Ok
    );
    let mut via_c = EncodedTxV1::boxed();
    let code = unsafe {
        crate::ffi::txf_encode_v1(raw.as_ptr(), raw.len(), &mut scratch, via_c.as_mut())
    };
    assert_eq!(code, 0);
    assert_eq!(via_c.header, via_rust.header);
    assert_eq!(via_c.header.requested_cu_limit, 1000);
    let null_code = unsafe {
        crate::ffi::txf_encode_v1(std::ptr::null(), 4, &mut scratch, via_c.as_mut())
    };
    assert_eq!(null_code, Status::NullArgument as i32);
    assert_eq!(
        crate::ffi::txf_encoded_v1_bytes(),
        std::mem::size_of::<EncodedTxV1>()
    );
    assert_eq!(
        crate::ffi::txf_scratch_bytes(),
        std::mem::size_of::<Scratch>()
    );
}

struct Lcg(u64);
impl Lcg {
    fn new(seed: u64) -> Self {
        Self(seed)
    }
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1);
        self.0
    }
}

pub(crate) fn json_str(line: &str, key: &str) -> String {
    let pat = format!("\"{key}\": \"");
    let i = line.find(&pat).unwrap_or_else(|| panic!("missing {key}"));
    let rest = &line[i + pat.len()..];
    let end = rest.find('"').unwrap();
    rest[..end].to_string()
}

fn json_u64(line: &str, key: &str) -> u64 {
    let pat = format!("\"{key}\": ");
    let i = line.find(&pat).unwrap_or_else(|| panic!("missing {key}"));
    let rest = &line[i + pat.len()..];
    let end = rest.find([',', '}']).unwrap();
    rest[..end].trim().parse().unwrap()
}

pub(crate) fn decode_hex(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    assert_eq!(b.len() % 2, 0);
    let mut out = Vec::with_capacity(b.len() / 2);
    for chunk in b.chunks(2) {
        out.push((hex(chunk[0]) << 4) | hex(chunk[1]));
    }
    out
}

fn hex(c: u8) -> u8 {
    match c {
        b'0'..=b'9' => c - b'0',
        b'a'..=b'f' => c - b'a' + 10,
        b'A'..=b'F' => c - b'A' + 10,
        _ => panic!("bad hex"),
    }
}
