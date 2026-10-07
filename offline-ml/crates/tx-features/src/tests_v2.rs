//! framed-tx-v2 tests: the v1 transaction format, and v2 == v1 on legacy/v0.

use crate::compute_budget::COMPUTE_BUDGET_PROGRAM_ID;
use crate::fingerprint::identity_hash64;
use crate::limits::{
    FEATURE_SCHEMA_V2_ID, ORIGIN_ALT_READONLY, ORIGIN_ALT_WRITABLE, PROGRAM_STATIC, V1_CONFIG_COMPUTE_UNIT_LIMIT, V1_CONFIG_HEAP_SIZE,
    V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE, V1_CONFIG_PRIORITY_FEE, VERSION_V1,
};
use crate::reference::encode_reference_v2;
use crate::tests::{assert_invariants_with, build, decode_hex, key_byte, BuiltIx, BuiltLookup};
use crate::view::{EncodedTxV1, EncodedTxV2, Scratch, TxExtV2, TxHeaderV1};
use crate::{encode_transaction_v1, encode_transaction_v2, Status};

#[derive(Clone, Copy, Default)]
struct Config {
    fee: Option<u64>,
    cu: Option<u32>,
    data: Option<u32>,
    heap: Option<u32>,
}

/// Serialize a v1-format transaction exactly as solana-transaction 4.x does:
/// `0x81 | header | mask | lifetime | n_ix | n_keys | keys | config values |
/// ix headers | ix accounts+data | signatures`.
fn build_v1(required: u8, ro_s: u8, ro_u: u8, keys: &[[u8; 32]], ixs: &[BuiltIx], cfg: Config) -> Vec<u8> {
    let mut mask = 0u32;
    if cfg.fee.is_some() {
        mask |= V1_CONFIG_PRIORITY_FEE;
    }
    if cfg.cu.is_some() {
        mask |= V1_CONFIG_COMPUTE_UNIT_LIMIT;
    }
    if cfg.data.is_some() {
        mask |= V1_CONFIG_LOADED_ACCOUNTS_DATA_SIZE;
    }
    if cfg.heap.is_some() {
        mask |= V1_CONFIG_HEAP_SIZE;
    }
    let mut b = vec![0x81, required, ro_s, ro_u];
    b.extend(mask.to_le_bytes());
    b.extend([0xCD; 32]);
    b.extend([ixs.len() as u8, keys.len() as u8]);
    for k in keys {
        b.extend_from_slice(k);
    }
    if let Some(v) = cfg.fee {
        b.extend(v.to_le_bytes());
    }
    for v in [cfg.cu, cfg.data, cfg.heap].into_iter().flatten() {
        b.extend(v.to_le_bytes());
    }
    for ix in ixs {
        b.extend([ix.program, ix.accounts.len() as u8]);
        b.extend((ix.data.len() as u16).to_le_bytes());
    }
    for ix in ixs {
        b.extend_from_slice(&ix.accounts);
        b.extend_from_slice(&ix.data);
    }
    for s in 0..required {
        b.extend(std::iter::repeat(0x51 + s).take(64));
    }
    b
}

fn ix(program: u8, accounts: &[u8], data: &[u8]) -> BuiltIx {
    BuiltIx { program, accounts: accounts.to_vec(), data: data.to_vec() }
}

/// Fast vs independent reference, then the shared invariants.
fn run_v2(raw: &[u8]) -> (Status, Box<EncodedTxV2>) {
    let mut scratch = Scratch::new();
    let mut fast = EncodedTxV2::boxed();
    let st = encode_transaction_v2(raw, &mut scratch, &mut fast);
    let mut slow = EncodedTxV2::boxed();
    let st2 = encode_reference_v2(raw, &mut slow);
    assert_eq!(st, st2, "fast/reference v2 status diverge on {} bytes", raw.len());
    if st == Status::Ok {
        assert_eq!(fast.base.header, slow.base.header);
        assert_eq!(fast.ext, slow.ext);
        let n = fast.base.header.instruction_count as usize;
        let m = fast.base.header.total_account_reference_count as usize;
        assert_eq!(&fast.base.instructions[..n], &slow.base.instructions[..n]);
        assert_eq!(&fast.base.accounts[..m], &slow.base.accounts[..m]);
        assert_invariants_with(&fast.base, raw, FEATURE_SCHEMA_V2_ID);
    } else {
        assert_eq!(fast.base.header, TxHeaderV1::zero());
        assert_eq!(fast.ext, TxExtV2::zero());
    }
    (st, fast)
}

/// v2 on legacy/v0 bytes: the v1 encoding with only `schema_id` changed, except
/// that with two or more lookup tables the loaded rows follow the runtime order
/// (v1 interleaves per table, a documented erratum). Returns true when the
/// transaction has two or more lookup tables.
fn assert_v2_matches_v1(raw: &[u8]) -> bool {
    let mut scratch = Scratch::new();
    let mut v1 = EncodedTxV1::boxed();
    let s1 = encode_transaction_v1(raw, &mut scratch, &mut v1);
    let (s2, v2) = run_v2(raw);
    assert_eq!(s1, s2);
    if s1 != Status::Ok {
        return false;
    }
    assert_eq!(v2.ext, TxExtV2::zero());
    assert_eq!(v2.base.header.schema_id, FEATURE_SCHEMA_V2_ID);
    let n = v1.header.instruction_count as usize;
    let m = v1.header.total_account_reference_count as usize;
    let s = v1.header.static_account_count as usize;
    if v1.header.alt_lookup_count < 2 {
        let mut h = v2.base.header;
        h.schema_id = v1.header.schema_id;
        assert_eq!(h, v1.header);
        assert_eq!(&v2.base.instructions[..n], &v1.instructions[..n]);
        assert_eq!(&v2.base.accounts[..m], &v1.accounts[..m]);
        return false;
    }
    // Static rows identical; loaded rows the same set, in runtime order.
    assert_eq!(&v2.base.accounts[..s], &v1.accounts[..s]);
    let key = |a: &crate::view::AccountRoleV1| (a.origin, a.alt_table_ordinal, a.alt_address_index, a.alt_identity_hash64);
    let mut a: Vec<_> = v1.accounts[s..m].iter().map(key).collect();
    let mut b: Vec<_> = v2.base.accounts[s..m].iter().map(key).collect();
    let w = v2.base.header.alt_loaded_writable_count as usize;
    assert!(b[..w].iter().all(|r| r.0 == ORIGIN_ALT_WRITABLE) && b[w..].iter().all(|r| r.0 == ORIGIN_ALT_READONLY));
    assert!(b[..w].windows(2).all(|p| p[0].1 <= p[1].1) && b[w..].windows(2).all(|p| p[0].1 <= p[1].1));
    a.sort();
    b.sort();
    assert_eq!(a, b);
    true
}

#[test]
fn layout_of_v2() {
    assert_eq!(std::mem::size_of::<TxExtV2>(), 24);
    assert_eq!(std::mem::offset_of!(EncodedTxV2, ext), std::mem::size_of::<EncodedTxV1>());
}

/// The `raw_hex` string of a JSONL row, with or without spaces after the colon.
fn raw_hex(line: &str) -> &str {
    let after = &line[line.find("\"raw_hex\"").expect("raw_hex") + 9..];
    let start = after.find('"').expect("value") + 1;
    let end = start + after[start..].find('"').expect("end");
    &after[start..end]
}

#[test]
fn v2_equals_v1_on_legacy_and_v0_corpus() {
    let root = concat!(env!("CARGO_MANIFEST_DIR"), "/../../");
    let (mut n, mut multi) = (0, 0);
    for name in ["raw-tx-snippet.jsonl", "raw-tx-100k.jsonl"] {
        let Ok(text) = std::fs::read_to_string(format!("{root}{name}")) else { continue };
        for line in text.lines().filter(|l| !l.trim().is_empty()) {
            multi += assert_v2_matches_v1(&decode_hex(raw_hex(line))) as usize;
            n += 1;
        }
    }
    assert!(n >= 8, "no corpus found");
    eprintln!("corpus: {n} transactions, {multi} with two or more lookup tables");
    // Synthetic legacy and v0 shapes, including an empty message.
    let keys = [key_byte(1), key_byte(2), key_byte(3)];
    for v0 in [false, true] {
        assert!(!assert_v2_matches_v1(&build(v0, 1, 1, 0, 1, &keys, &[ix(2, &[0, 1], &[9, 9])], &[])));
        assert!(!assert_v2_matches_v1(&build(v0, 1, 1, 0, 0, &keys[..1], &[], &[])));
    }
}

#[test]
fn v1_format_shape_and_config() {
    let payer = key_byte(1);
    let pool = key_byte(4);
    let program = key_byte(2);
    let cb = COMPUTE_BUDGET_PROGRAM_ID;
    let keys = [payer, pool, program, cb];
    // payer signer+writable, pool writable, program and ComputeBudget readonly.
    let ixs = [
        ix(3, &[], &[2, 0x40, 0x0d, 0x03, 0x00]), // ComputeBudget SetComputeUnitLimit(200_000)
        ix(2, &[0, 1, 1], &[0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 1, 2, 3]),
    ];
    let cfg = Config { fee: Some(5_000), cu: Some(300_000), data: Some(65_536), heap: Some(64 * 1024) };
    let raw = build_v1(1, 0, 2, &keys, &ixs, cfg);
    let (st, out) = run_v2(&raw);
    assert_eq!(st, Status::Ok);
    let h = &out.base.header;
    assert_eq!(h.version, VERSION_V1);
    assert_eq!((h.signature_count, h.required_signature_count, h.static_account_count), (1, 1, 4));
    assert_eq!((h.static_writable_count, h.static_readonly_count), (2, 2));
    assert_eq!((h.total_account_reference_count, h.alt_lookup_count, h.uses_alt), (4, 0, 0));
    assert_eq!(h.instruction_count, 2);
    // ComputeBudget instructions keep their framed-tx-v1 meaning; config lives in ext.
    assert_eq!((h.compute_budget_instruction_count, h.requested_cu_limit), (1, 200_000));
    assert_eq!(
        out.ext,
        TxExtV2 {
            config_priority_fee_lamports: 5_000,
            config_mask: 0b11111,
            config_compute_unit_limit: 300_000,
            config_loaded_accounts_data_size: 65_536,
            config_heap_size: 64 * 1024,
        }
    );
    let swap = &out.base.instructions[1];
    assert_eq!(swap.program_ref_kind, PROGRAM_STATIC);
    assert_eq!(swap.program_identity_hash64, identity_hash64(&program));
    assert_eq!((swap.account_count, swap.writable_account_count, swap.signer_account_count), (3, 3, 1));
    assert_eq!((swap.data_len, swap.prefix1, swap.reused_account_count), (9, 0xAA, 1));
    let a = &out.base.accounts;
    assert_eq!((a[0].fee_payer, a[0].signer, a[0].writable), (1, 1, 1));
    assert_eq!((a[1].writable, a[1].usage_count), (1, 1));
    assert_eq!((a[2].writable, a[2].used_as_program), (0, 1));
}

#[test]
fn v1_config_subsets_and_multiple_signers() {
    let keys = [key_byte(1), key_byte(5), key_byte(6), key_byte(2)];
    let ixs = [ix(3, &[0, 1, 2], &[1, 2, 3, 4])];
    for bits in 0u8..16 {
        let cfg = Config {
            fee: (bits & 1 != 0).then_some(7),
            cu: (bits & 2 != 0).then_some(11),
            data: (bits & 4 != 0).then_some(13),
            heap: (bits & 8 != 0).then_some(32 * 1024),
        };
        // Two signers (second readonly), one writable unsigned, program readonly.
        let raw = build_v1(2, 1, 1, &keys, &ixs, cfg);
        let (st, out) = run_v2(&raw);
        assert_eq!(st, Status::Ok, "config bits {bits:04b}");
        assert_eq!(out.ext.config_priority_fee_lamports, cfg.fee.unwrap_or(0));
        assert_eq!(out.ext.config_compute_unit_limit, cfg.cu.unwrap_or(0));
        assert_eq!(out.ext.config_loaded_accounts_data_size, cfg.data.unwrap_or(0));
        assert_eq!(out.ext.config_heap_size, cfg.heap.unwrap_or(0));
        let h = &out.base.header;
        assert_eq!((h.signature_count, h.readonly_signed_count, h.static_writable_count), (2, 1, 2));
        assert_eq!(out.base.instructions[0].signer_account_count, 2);
    }
}

#[test]
fn v1_malformed_and_unsupported() {
    let keys = [key_byte(1), key_byte(2)];
    let good = build_v1(1, 0, 1, &keys, &[ix(1, &[0], &[1, 2])], Config { cu: Some(5), ..Config::default() });
    assert_eq!(run_v2(&good).0, Status::Ok);
    // Every truncation fails closed (and never panics), as does any trailing byte.
    for cut in 0..good.len() {
        assert_ne!(run_v2(&good[..cut]).0, Status::Ok, "cut {cut}");
    }
    let mut extra = good.clone();
    extra.push(0);
    assert_eq!(run_v2(&extra).0, Status::Malformed);
    // Half a priority-fee bit pair, or an unknown bit.
    for bad_mask in [0b01u32, 0b10, 0b100000, 1 << 31] {
        let mut raw = good.clone();
        raw[4..8].copy_from_slice(&bad_mask.to_le_bytes());
        assert_eq!(run_v2(&raw).0, Status::Malformed, "mask {bad_mask:#x}");
    }
    // Header counts that do not fit the keys.
    assert_eq!(run_v2(&build_v1(1, 2, 0, &keys, &[], Config::default())).0, Status::Malformed);
    assert_eq!(run_v2(&build_v1(3, 0, 0, &keys, &[], Config::default())).0, Status::Malformed);
    // Account and program indexes out of range.
    assert_eq!(run_v2(&build_v1(1, 0, 1, &keys, &[ix(1, &[7], &[])], Config::default())).0, Status::Malformed);
    assert_eq!(run_v2(&build_v1(1, 0, 1, &keys, &[ix(9, &[0], &[])], Config::default())).0, Status::Malformed);
    // Other versioned prefixes are unsupported; framed-tx-v1 never parses v1 bytes.
    for prefix in [0x80u8, 0x82, 0xFF] {
        let mut raw = good.clone();
        raw[0] = prefix;
        assert_eq!(run_v2(&raw).0, Status::UnsupportedVersion, "prefix {prefix:#x}");
    }
    let mut scratch = Scratch::new();
    let mut v1 = EncodedTxV1::boxed();
    assert_ne!(encode_transaction_v1(&good, &mut scratch, &mut v1), Status::Ok);
    assert_eq!(run_v2(&[]).0, Status::Malformed);
}

#[test]
fn v1_capacity_edges() {
    // 64 keys and 64 instructions: the v1 protocol maxima.
    let keys: Vec<[u8; 32]> = (0..64u8).map(|i| key_byte(i.wrapping_add(10))).collect();
    let ixs: Vec<BuiltIx> = (0..64u8).map(|i| ix(1 + (i % 63), &[0, i % 64], &[i; 3])).collect();
    let (st, out) = run_v2(&build_v1(1, 0, 10, &keys, &ixs, Config::default()));
    assert_eq!(st, Status::Ok);
    assert_eq!((out.base.header.static_account_count, out.base.header.instruction_count), (64, 64));
}

#[test]
fn fuzz_v2_never_panics_matches_reference_and_is_deterministic() {
    let keys = [key_byte(1), key_byte(2), key_byte(3), COMPUTE_BUDGET_PROGRAM_ID];
    let seeds = [
        build_v1(1, 0, 2, &keys, &[ix(2, &[0, 1], &[3, 4, 5]), ix(3, &[], &[2, 1, 0, 0, 0])], Config { fee: Some(9), cu: Some(1), ..Config::default() }),
        build(false, 1, 1, 0, 2, &keys, &[ix(2, &[0, 1], &[3, 4, 5])], &[]),
        build(true, 1, 1, 0, 2, &keys, &[ix(3, &[], &[3, 1, 0, 0, 0, 0, 0, 0, 0])], &[]),
    ];
    let mut state = 0x9E37_79B9_7F4A_7C15u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    let mut scratch = Scratch::new();
    let mut a = EncodedTxV2::boxed();
    let mut b = EncodedTxV2::boxed();
    for _ in 0..20_000 {
        let mut raw = seeds[(next() % 3) as usize].clone();
        for _ in 0..(1 + next() % 4) {
            let i = (next() as usize) % raw.len();
            raw[i] = next() as u8;
        }
        let (st, _) = run_v2(&raw);
        let s1 = encode_transaction_v2(&raw, &mut scratch, &mut a);
        let s2 = encode_transaction_v2(&raw, &mut scratch, &mut b);
        assert_eq!((s1, s2), (st, st));
        if st == Status::Ok {
            assert_eq!(a.base.header, b.base.header);
            assert_eq!(a.ext, b.ext);
        }
    }
}

#[test]
fn ffi_v2_matches_rust() {
    let keys = [key_byte(1), key_byte(2)];
    let raw = build_v1(1, 0, 1, &keys, &[ix(1, &[0], &[1, 2])], Config { fee: Some(77), ..Config::default() });
    let mut scratch = Scratch::new();
    let mut rust = EncodedTxV2::boxed();
    assert_eq!(encode_transaction_v2(&raw, &mut scratch, &mut rust), Status::Ok);
    let mut c_scratch = Scratch::new();
    let mut c_out = EncodedTxV2::boxed();
    let code = unsafe { crate::ffi::txf_encode_v2(raw.as_ptr(), raw.len(), &mut c_scratch, &mut *c_out) };
    assert_eq!(code, Status::Ok.as_i32());
    assert_eq!(c_out.base.header, rust.base.header);
    assert_eq!(c_out.ext, rust.ext);
    let null = unsafe { crate::ffi::txf_encode_v2(raw.as_ptr(), raw.len(), std::ptr::null_mut(), &mut *c_out) };
    assert_eq!(null, Status::NullArgument.as_i32());
    assert_eq!(crate::ffi::txf_encoded_v2_bytes(), std::mem::size_of::<EncodedTxV2>());
}

#[test]
fn v0_with_two_lookup_tables_uses_runtime_order() {
    let keys = [key_byte(1), key_byte(2)];
    let lookups = [
        BuiltLookup { key: key_byte(40), writable: vec![1, 2], readonly: vec![3] },
        BuiltLookup { key: key_byte(41), writable: vec![4], readonly: vec![5, 6] },
    ];
    // Runtime expansion: static 0,1 | T0.w1 T0.w2 T1.w4 | T0.r3 T1.r5 T1.r6.
    // Index 4 is T1's writable entry; framed-tx-v1 would put T0.r3 there.
    let raw = build(true, 1, 1, 0, 1, &keys, &[ix(1, &[0, 4, 5], &[7])], &lookups);
    let (st, out) = run_v2(&raw);
    assert_eq!(st, Status::Ok);
    let rows: Vec<(u8, u16, u8)> =
        out.base.accounts[2..8].iter().map(|a| (a.origin, a.alt_table_ordinal, a.alt_address_index)).collect();
    let (w, r) = (ORIGIN_ALT_WRITABLE, ORIGIN_ALT_READONLY);
    assert_eq!(rows, vec![(w, 0, 1), (w, 0, 2), (w, 1, 4), (r, 0, 3), (r, 1, 5), (r, 1, 6)]);
    assert_eq!(out.base.instructions[0].writable_account_count, 2); // payer + T1.w4; T0.r3 is index 5
    let mut scratch = Scratch::new();
    let mut v1 = EncodedTxV1::boxed();
    assert_eq!(encode_transaction_v1(&raw, &mut scratch, &mut v1), Status::Ok);
    assert_eq!((v1.accounts[4].origin, v1.accounts[4].alt_address_index), (r, 3)); // the v1 erratum
}
