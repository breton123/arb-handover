//! Historical JSONL bytes and a live framed buffer call the same encoder.
//!
//! The canonical populated image must be identical.

use tx_features::{encode_transaction_v1, write_canonical, EncodedTxV1, Scratch, Status, FEATURE_SCHEMA};

fn main() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../raw-tx-snippet.jsonl");
    let text = std::fs::read_to_string(path).expect("pilot jsonl");
    let line = text.lines().find(|l| !l.trim().is_empty()).expect("row");
    let raw = decode_hex(&json_str(line, "raw_hex"));

    let mut scratch = Scratch::new();
    let mut historical = EncodedTxV1::boxed();
    let status = encode_transaction_v1(&raw, &mut scratch, &mut historical);
    assert_eq!(status, Status::Ok);

    // Live path: the NIC/shred framer already owns the serialized bytes.
    let mut framed = vec![0u8; raw.len()];
    framed.copy_from_slice(&raw);
    let mut live = EncodedTxV1::boxed();
    let status = encode_transaction_v1(&framed, &mut scratch, &mut live);
    assert_eq!(status, Status::Ok);

    let mut left = vec![0u8; 1 << 20];
    let mut right = vec![0u8; 1 << 20];
    let n_left = write_canonical(&historical, &mut left).unwrap();
    let n_right = write_canonical(&live, &mut right).unwrap();
    assert_eq!(n_left, n_right);
    assert_eq!(&left[..n_left], &right[..n_right]);

    println!(
        "{FEATURE_SCHEMA}: historical and live canonical image matches ({n_left} bytes), version={} instructions={} cu_limit={}",
        historical.header.version,
        historical.header.instruction_count,
        historical.header.requested_cu_limit
    );
}

fn json_str(line: &str, key: &str) -> String {
    let pat = format!("\"{key}\": \"");
    let i = line.find(&pat).unwrap();
    let rest = &line[i + pat.len()..];
    let end = rest.find('"').unwrap();
    rest[..end].to_string()
}

fn decode_hex(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    (0..b.len() / 2)
        .map(|i| (hex(b[i * 2]) << 4) | hex(b[i * 2 + 1]))
        .collect()
}

fn hex(c: u8) -> u8 {
    match c {
        b'0'..=b'9' => c - b'0',
        b'a'..=b'f' => c - b'a' + 10,
        _ => panic!("hex"),
    }
}
