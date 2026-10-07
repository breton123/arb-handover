//! framed-tx-v2 against the fleet collector's own decoded records.
//!
//! Each dataset row stores `raw` plus the collector's decoding of it
//! (`wincode::deserialize::<VersionedTransaction>`, round-trip checked): header,
//! static keys, per-key signer/writable, and per-instruction program, account
//! indexes, data and flags. This test encodes every `raw` with
//! `encode_transaction_v2` and checks each of those fields.
//!
//! Writability: framed-tx-v1/v2 define it from the message header (and ALT
//! lists), as the spec says. The collector's flags are the runtime view, which
//! demotes some header-writable keys (program ids, reserved sysvars/builtins).
//! So runtime-writable must imply header-writable; demotions are counted.
//!
//! Opt-in: set `FIRM_ML_DATASET_PART` to one or more `part-*.transactions.parquet`
//! paths (separated by `;`). Without it the test only exercises the decoder.

use std::path::Path;

use arrow_array::cast::AsArray;
use ml_prep::pq::{self, Cols};
use tx_features::{encode_transaction_v2, identity_hash64, EncodedTxV2, Scratch, Status, VERSION_LEGACY, VERSION_V0, VERSION_V1};

#[derive(Debug, Clone)]
enum V {
    U(u64),
    #[allow(dead_code)]
    N(i64),
    B(Vec<u8>),
    T(String),
    A(Vec<V>),
    M(Vec<(V, V)>),
    Bool(bool),
    Null,
    F,
}

impl V {
    fn get(&self, key: &str) -> &V {
        match self {
            V::M(kv) => kv.iter().find(|(k, _)| matches!(k, V::T(t) if t == key)).map(|(_, v)| v).unwrap_or(&V::Null),
            _ => &V::Null,
        }
    }
    fn arr(&self) -> &[V] {
        match self {
            V::A(a) => a,
            _ => &[],
        }
    }
    fn text(&self) -> &str {
        match self {
            V::T(t) => t,
            _ => panic!("not text: {self:?}"),
        }
    }
    fn u(&self) -> u64 {
        match self {
            V::U(u) => *u,
            _ => panic!("not uint: {self:?}"),
        }
    }
    fn bytes(&self) -> Vec<u8> {
        match self {
            V::B(b) => b.clone(),
            V::A(a) => a.iter().map(|x| x.u() as u8).collect(),
            _ => panic!("not bytes: {self:?}"),
        }
    }
    fn flag(&self) -> bool {
        matches!(self, V::Bool(true))
    }
}

/// Minimal CBOR decoder (definite and indefinite lengths) for the test only.
fn decode(b: &[u8], i: &mut usize) -> V {
    let ib = b[*i];
    *i += 1;
    let (major, ai) = (ib >> 5, ib & 31);
    let arg = |i: &mut usize| -> Option<u64> {
        let n = match ai {
            0..=23 => return Some(ai as u64),
            24 => 1,
            25 => 2,
            26 => 4,
            27 => 8,
            31 => return None,
            _ => panic!("bad cbor"),
        };
        let v = b[*i..*i + n].iter().fold(0u64, |a, x| (a << 8) | *x as u64);
        *i += n;
        Some(v)
    };
    let len = arg(i);
    match major {
        0 => V::U(len.unwrap()),
        1 => V::N(-1 - len.unwrap() as i64),
        2 | 3 => {
            let mut out = Vec::new();
            match len {
                Some(n) => {
                    out.extend(&b[*i..*i + n as usize]);
                    *i += n as usize;
                }
                None => {
                    while b[*i] != 0xff {
                        match decode(b, i) {
                            V::B(c) => out.extend(c),
                            V::T(t) => out.extend(t.into_bytes()),
                            _ => panic!("bad chunk"),
                        }
                    }
                    *i += 1;
                }
            }
            if major == 2 { V::B(out) } else { V::T(String::from_utf8(out).unwrap()) }
        }
        4 => {
            let mut v = Vec::new();
            match len {
                Some(n) => (0..n).for_each(|_| v.push(decode(b, i))),
                None => {
                    while b[*i] != 0xff {
                        v.push(decode(b, i));
                    }
                    *i += 1;
                }
            }
            V::A(v)
        }
        5 => {
            let mut v = Vec::new();
            match len {
                Some(n) => (0..n).for_each(|_| {
                    let k = decode(b, i);
                    v.push((k, decode(b, i)));
                }),
                None => {
                    while b[*i] != 0xff {
                        let k = decode(b, i);
                        v.push((k, decode(b, i)));
                    }
                    *i += 1;
                }
            }
            V::M(v)
        }
        6 => decode(b, i),
        7 => match ai {
            20 => V::Bool(false),
            21 => V::Bool(true),
            22 | 23 => V::Null,
            _ => V::F,
        },
        _ => unreachable!(),
    }
}

#[test]
fn decoder_reads_nested_records() {
    // {"a": [1, h'02', {"b": true}], "c": null}
    let b = [0xa2, 0x61, b'a', 0x83, 0x01, 0x41, 0x02, 0xa1, 0x61, b'b', 0xf5, 0x61, b'c', 0xf6];
    let v = decode(&b, &mut 0);
    assert_eq!(v.get("a").arr()[1].bytes(), vec![2]);
    assert!(v.get("a").arr()[2].get("b").flag());
}

#[test]
fn v2_matches_collector_decoding_on_real_partitions() {
    let Ok(paths) = std::env::var("FIRM_ML_DATASET_PART") else {
        eprintln!("FIRM_ML_DATASET_PART not set; skipping real-data parity");
        return;
    };
    let mut scratch = Scratch::new();
    let mut out = EncodedTxV2::boxed();
    let (mut checked, mut v1_rows, mut demoted) = (0u64, 0u64, 0u64);
    for path in paths.split(';').filter(|p| !p.is_empty()) {
        let (reader, order) = pq::open(Path::new(path), &["record_cbor"]).unwrap();
        for batch in reader {
            let batch = batch.unwrap();
            let c = Cols { batch: &batch, order: &order };
            let _ = &c;
            let records = batch.column(order[0]).as_binary::<i32>();
            for r in 0..batch.num_rows() {
                let rec = decode(records.value(r), &mut 0);
                let raw = rec.get("raw").bytes();
                let ctx = format!("{path} row {r} sig {}", rec.get("signature").text());
                assert_eq!(encode_transaction_v2(&raw, &mut scratch, &mut out), Status::Ok, "{ctx}");
                let h = &out.base.header;
                let want_version = match rec.get("version").text() {
                    "Legacy(Legacy)" => VERSION_LEGACY,
                    "Number(0)" => VERSION_V0,
                    "Number(1)" => VERSION_V1,
                    other => panic!("{ctx}: version {other}"),
                };
                assert_eq!(h.version, want_version, "{ctx}");
                v1_rows += (want_version == VERSION_V1) as u64;
                let hdr: Vec<u64> = rec.get("header").arr().iter().map(V::u).collect();
                assert_eq!(
                    (h.required_signature_count as u64, h.readonly_signed_count as u64, h.readonly_unsigned_count as u64),
                    (hdr[0], hdr[1], hdr[2]),
                    "{ctx}"
                );
                let keys = rec.get("static_keys").arr();
                assert_eq!(h.static_account_count as usize, keys.len(), "{ctx}");
                let (signer, writable) = (rec.get("signer").arr(), rec.get("writable").arr());
                for (k, key) in keys.iter().enumerate() {
                    let bytes = bs58::decode(key.text()).into_vec().unwrap();
                    let a = &out.base.accounts[k];
                    assert_eq!(a.identity_hash64, identity_hash64(&bytes), "{ctx} key {k}");
                    assert_eq!(a.signer == 1, signer[k].flag(), "{ctx} signer {k}");
                    assert!(a.writable == 1 || !writable[k].flag(), "{ctx} runtime-writable key {k} not header-writable");
                    demoted += (a.writable == 1 && !writable[k].flag()) as u64;
                }
                if want_version == VERSION_V1 {
                    assert_eq!((h.alt_lookup_count, h.total_account_reference_count as usize), (0, keys.len()), "{ctx}");
                }
                let ixs = rec.get("instructions").arr();
                assert_eq!(h.instruction_count as usize, ixs.len(), "{ctx}");
                for (n, rix) in ixs.iter().enumerate() {
                    let ix = &out.base.instructions[n];
                    let accounts = rix.get("accounts").bytes();
                    let data = rix.get("data").bytes();
                    let off = ix.accounts_offset as usize;
                    assert_eq!(&raw[off..off + ix.account_count as usize], &accounts[..], "{ctx} ix {n} accounts");
                    assert_eq!(ix.data_len as usize, data.len(), "{ctx} ix {n} data_len");
                    let d = ix.data_offset as usize;
                    assert_eq!(&raw[d..d + data.len()], &data[..], "{ctx} ix {n} data");
                    let hn = ix.head_len as usize;
                    assert_eq!(&ix.head[..hn], &data[..hn], "{ctx} ix {n} head");
                    if (ix.program_account_index as usize) < keys.len() {
                        let program = bs58::decode(rix.get("program_id").text()).into_vec().unwrap();
                        assert_eq!(ix.program_identity_hash64, identity_hash64(&program), "{ctx} ix {n} program");
                    }
                    // Per listed entry: signer must match; runtime-writable implies header-writable.
                    let (rs, rw) = (rix.get("signer").arr(), rix.get("writable").arr());
                    for (e, &acct) in accounts.iter().enumerate() {
                        let a = &out.base.accounts[acct as usize];
                        assert_eq!(a.signer == 1, rs[e].flag(), "{ctx} ix {n} entry {e} signer");
                        assert!(a.writable == 1 || !rw[e].flag(), "{ctx} ix {n} entry {e} writable");
                    }
                }
                checked += 1;
            }
        }
    }
    eprintln!("real-data parity: {checked} transactions, {v1_rows} in the v1 format, {demoted} runtime-demoted writable keys");
    assert!(checked > 0 && v1_rows > 0);
}
