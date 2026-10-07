//! End-to-end tests of `ml-prep build` on a synthetic label / candidate-library
//! fixture built around the eight real transactions in `raw-tx-snippet.jsonl`.

use std::collections::BTreeMap;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow_array::{
    Array, ArrayRef, BooleanArray, Int64Array, Int8Array, RecordBatch, StringArray, UInt32Array, UInt64Array, UInt8Array,
};
use arrow_schema::{Field, Schema};
use ml_prep::build::{self, BuildArgs};
use ml_prep::flatten;
use ml_prep::pq::{self, Cols};
use ml_prep::raw::{self, RawSource};
use parquet::arrow::ArrowWriter;
use tx_features::{encode_transaction_v2, EncodedTxV2, Scratch, Status};

const SNIPPET: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../raw-tx-snippet.jsonl");
const SLOT: u64 = 446457212;
const SOL: &str = "So11111111111111111111111111111111111111112";
const PROG_A: &str = "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc";
const PROG_B: &str = "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo";
const FAM_1: &str = "prefix8:f8c69e91e17587c8:len24:accounts15";
const FAM_2: &str = "prefix8:414b3f4ceb5b5b88:len28:accounts19";

fn snippet() -> Vec<(u64, String, Vec<u8>)> {
    std::fs::read_to_string(SNIPPET)
        .unwrap()
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| {
            let v: serde_json::Value = serde_json::from_str(l).unwrap();
            (
                v["tx_index"].as_u64().unwrap(),
                v["signature"].as_str().unwrap().to_string(),
                raw::decode_hex(v["raw_hex"].as_str().unwrap()).unwrap(),
            )
        })
        .collect()
}

fn write(path: &Path, cols: Vec<(&str, ArrayRef)>) {
    let fields: Vec<Field> = cols.iter().map(|(n, a)| Field::new(*n, a.data_type().clone(), true)).collect();
    let schema = Arc::new(Schema::new(fields));
    let batch = RecordBatch::try_new(schema.clone(), cols.into_iter().map(|(_, a)| a).collect()).unwrap();
    let mut w = ArrowWriter::try_new(File::create(path).unwrap(), schema, None).unwrap();
    w.write(&batch).unwrap();
    w.close().unwrap();
}

fn s(v: &[&str]) -> ArrayRef {
    Arc::new(StringArray::from(v.to_vec()))
}
fn u64s(v: &[u64]) -> ArrayRef {
    Arc::new(UInt64Array::from(v.to_vec()))
}
fn u32s(v: &[u32]) -> ArrayRef {
    Arc::new(UInt32Array::from(v.to_vec()))
}

/// One trigger row: (master, tx_index, program, family, layer, base, status, reason, profitable routes).
type Row = (u64, u64, &'static str, &'static str, &'static str, &'static str, &'static str, &'static str, &'static str);

/// Fixture transactions (snippet tx_index -> master_tx_id):
/// - 0 -> 1 POSITIVE. Profitable r1,r9. P1 (rank 0, TOP_LEVEL A/F1) covers r1; P3 (rank 1, same key)
///   does not; P2 (INNER B/F2) does not.
/// - 1 -> 2 CERTIFIED_NEGATIVE.
/// - 2 -> 3 one CERTIFIED_NEGATIVE row plus an UNKNOWN row with a base: must stay UNKNOWN.
/// - 3 -> 4 NO_LABEL (base "") on the A/F1 TOP_LEVEL key.
/// - 4 -> 5 POSITIVE, profitable r2, covered only by the INNER-key pack P2.
fn rows() -> Vec<Row> {
    vec![
        (1, 0, PROG_A, FAM_1, "TOP_LEVEL", SOL, "POSITIVE", "", "r1,r9"),
        (1, 0, PROG_B, FAM_2, "INNER", SOL, "POSITIVE", "", "r1,r9"),
        (2, 1, PROG_A, FAM_1, "TOP_LEVEL", SOL, "CERTIFIED_NEGATIVE", "", ""),
        (3, 2, PROG_A, FAM_1, "TOP_LEVEL", SOL, "CERTIFIED_NEGATIVE", "", ""),
        (3, 2, PROG_B, FAM_2, "INNER", SOL, "UNKNOWN", "UNRESOLVED", ""),
        (4, 3, PROG_A, FAM_1, "TOP_LEVEL", "", "UNKNOWN", "NO_LABEL", ""),
        (5, 4, PROG_B, FAM_2, "INNER", SOL, "POSITIVE", "", "r2"),
        (5, 4, PROG_A, FAM_1, "TOP_LEVEL", SOL, "POSITIVE", "", "r2"),
    ]
}

struct Fixture {
    labels: PathBuf,
    index: PathBuf,
    library: PathBuf,
    legacy: PathBuf,
}

/// Library trained on masters 1..=2: only master 1's routes count as seen in training.
const TRAIN: (u64, u64) = (1, 2);

fn fixture(dir: &Path, rows: &[Row]) -> Fixture {
    let labels = dir.join("labels");
    let index = dir.join("index");
    let library = dir.join("library");
    let legacy = dir.join("legacy");
    for d in [&labels, &index, &library, &legacy] {
        std::fs::create_dir_all(d).unwrap();
    }
    let sigs: BTreeMap<u64, String> = snippet().into_iter().map(|(i, s, _)| (i, s)).collect();
    let n = rows.len();
    let col = |f: &dyn Fn(&Row) -> &'static str| -> ArrayRef { s(&rows.iter().map(f).collect::<Vec<_>>()) };
    let best: ArrayRef = Arc::new(Int64Array::from(
        rows.iter().map(|r| if r.6 == "POSITIVE" { Some(1000) } else { None }).collect::<Vec<_>>(),
    ));
    write(
        &labels.join("trigger_occurrences.parquet"),
        vec![
            ("master_tx_id", u64s(&rows.iter().map(|r| r.0).collect::<Vec<_>>())),
            ("signature", Arc::new(StringArray::from_iter_values(rows.iter().map(|r| sigs[&r.1].clone())))),
            ("slot", u64s(&vec![SLOT; n])),
            ("entry_index", u64s(&rows.iter().map(|r| r.1 + 3).collect::<Vec<_>>())),
            ("tx_index", u64s(&rows.iter().map(|r| r.1).collect::<Vec<_>>())),
            ("base", col(&|r| r.5)),
            ("program_id", col(&|r| r.2)),
            ("family_id", col(&|r| r.3)),
            ("layer", col(&|r| r.4)),
            ("occurrence_index", u64s(&(0..n as u64).collect::<Vec<_>>())),
            ("label_status", col(&|r| r.6)),
            ("unknown_reason", col(&|r| r.7)),
            ("best_net_profit", best),
            ("label_quality", col(&|r| if r.5.is_empty() { "" } else { "CLEAN_MARGINAL" })),
        ],
    );
    // Positives index: one row per POSITIVE trigger row.
    let pos: Vec<(usize, &Row)> = rows.iter().enumerate().filter(|(_, r)| r.6 == "POSITIVE").collect();
    std::fs::write(index.join("library-index.json"), r#"{"schema":"candidate-library-index-v1"}"#).unwrap();
    write(
        &index.join("positives.parquet"),
        vec![
            ("master_tx_id", u64s(&pos.iter().map(|(_, r)| r.0).collect::<Vec<_>>())),
            ("occurrence_index", u64s(&pos.iter().map(|(i, _)| *i as u64).collect::<Vec<_>>())),
            ("program_id", s(&pos.iter().map(|(_, r)| r.2).collect::<Vec<_>>())),
            ("family_id", s(&pos.iter().map(|(_, r)| r.3).collect::<Vec<_>>())),
            ("layer", s(&pos.iter().map(|(_, r)| r.4).collect::<Vec<_>>())),
            ("base", s(&pos.iter().map(|(_, r)| r.5).collect::<Vec<_>>())),
            ("route_ids", s(&pos.iter().map(|(_, r)| r.8).collect::<Vec<_>>())),
        ],
    );
    // Library: P1 (rank 0), P3 (rank 1) on (A, F1, TOP_LEVEL, SOL); P2 (rank 0) on (B, F2, INNER, SOL).
    let ids = ["P1", "P2", "P3"];
    let three = |v: u64| u64s(&[v, v, v]);
    std::fs::write(
        library.join("library.json"),
        format!(r#"{{"schema":"candidate-library-v1","train_master_tx_id_min":{},"train_master_tx_id_max":{}}}"#, TRAIN.0, TRAIN.1),
    )
    .unwrap();
    write(
        &library.join("library_packs.parquet"),
        vec![
            ("pack_id", s(&ids)),
            ("program_id", s(&[PROG_A, PROG_B, PROG_A])),
            ("family_id", s(&[FAM_1, FAM_2, FAM_1])),
            ("layer", s(&["TOP_LEVEL", "INNER", "TOP_LEVEL"])),
            ("base", s(&[SOL, SOL, SOL])),
            ("rank", u32s(&[0, 0, 1])),
            ("route_ids", s(&["r1", "r2", "r3"])),
            ("account_set_id", s(&["a1", "a2", "a3"])),
            ("pool_ids", s(&["p1,p2", "p3", "p1"])),
            ("venue_sequences", s(&["v1", "v2", "v1"])),
            ("account_count", u32s(&[20, 25, 30])),
            ("writable_known", u32s(&[10, 12, 14])),
            ("writable_conservative", u32s(&[10, 12, 14])),
            ("estimated_message_bytes", u32s(&[700, 800, 900])),
            ("estimated_cu", u64s(&[300_000, 400_000, 200_000])),
            ("dynamic_requirements", u32s(&[2, 1, 1])),
            ("selection_gain", Arc::new(Int64Array::from(vec![Some(50), Some(40), Some(-3)]))),
            ("train_covered_txs", three(1)),
            ("train_new_txs", three(1)),
            ("train_cumulative_txs", three(1)),
            ("train_key_txs", three(1)),
            ("new_route_ids", three(1)),
            ("new_pools", three(1)),
            ("new_venue_sequences", three(1)),
            ("new_hop_counts", three(1)),
            ("shared_accounts", three(0)),
            ("route_support_min", three(1)),
            ("route_support_sum", three(1)),
            ("train_master_tx_id_min", three(TRAIN.0)),
            ("train_master_tx_id_max", three(TRAIN.1)),
        ],
    );
    // Legacy catalog with the same three packs.
    write(
        &legacy.join("candidate_packs.parquet"),
        vec![
            ("pack_id", s(&ids)),
            ("program_id", s(&[PROG_A, PROG_B, PROG_A])),
            ("family_id", s(&[FAM_1, FAM_2, FAM_1])),
            ("layer", s(&["TOP_LEVEL", "INNER", "TOP_LEVEL"])),
            ("base", s(&[SOL, SOL, SOL])),
            ("rank", u32s(&[0, 0, 1])),
            ("route_ids", s(&["r1", "r2", "r3"])),
            ("account_set_id", s(&["a1", "a2", "a3"])),
            ("account_count", u64s(&[20, 25, 30])),
            ("writable_count", u64s(&[10, 12, 14])),
            ("writable_conservative_count", u64s(&[10, 12, 14])),
            ("dynamic_groups", s(&["CLMM_TICK_ARRAY:x,USER_TOKEN_ACCOUNTS", "DLMM_BIN_ARRAY:y", "USER_TOKEN_ACCOUNTS"])),
            ("dynamic_count", u64s(&[2, 1, 1])),
            ("estimated_message_bytes", u64s(&[700, 800, 900])),
            ("message_known", Arc::new(BooleanArray::from(vec![true, true, true]))),
            ("estimated_cu", u64s(&[300_000, 400_000, 200_000])),
            ("cu_known", Arc::new(BooleanArray::from(vec![true, true, true]))),
            ("train_master_tx_id_min", three(TRAIN.0)),
            ("train_master_tx_id_max", three(TRAIN.1)),
        ],
    );
    write(
        &labels.join("route_catalog.parquet"),
        vec![
            ("route_id", s(&["r1", "r2", "r3", "r9"])),
            ("hops", Arc::new(UInt8Array::from(vec![2u8, 3, 2, 4]))),
            ("program_ids", s(&[&format!("{PROG_A},{PROG_A}"), &format!("{PROG_B},{PROG_A},{PROG_B}"), &format!("{PROG_A},{PROG_B}"), PROG_A])),
            ("economic_families", s(&["CLMM,CLMM", "DLMM,CLMM,DLMM", "CLMM,DLMM", "CLMM"])),
        ],
    );
    // Tracked universe: one pool on PROG_A whose vault is a writable static key of snippet tx 0.
    let pool = bs58::encode([7u8; 32]).into_string();
    let vault = tracked_vault();
    write(&labels.join("markets.parquet"), vec![("pool", s(&[&pool])), ("program", s(&[PROG_A]))]);
    write(
        &labels.join("execution_catalog.parquet"),
        vec![
            ("pool", s(&[&pool, &pool, &pool])),
            ("role", s(&["VAULT", "MINT", "PROGRAM"])),
            // The mint is shared, not market-specific: it must not make anything relevant.
            ("pubkey", s(&[&vault, SOL, PROG_A])),
        ],
    );
    Fixture { labels, index, library, legacy }
}

/// Static account keys straight from the wire (independent of the encoder):
/// (pubkey, writable) for each static key.
fn static_keys(raw: &[u8]) -> Vec<(String, bool)> {
    let mut i = 0;
    let nsig = raw[i] as usize; // < 128 in the snippet
    i += 1 + 64 * nsig;
    if raw[i] & 0x80 != 0 {
        i += 1; // v0 prefix
    }
    let (req, ro_signed, ro_unsigned) = (raw[i] as usize, raw[i + 1] as usize, raw[i + 2] as usize);
    i += 3;
    let n = raw[i] as usize; // < 128 in the snippet
    i += 1;
    (0..n)
        .map(|k| {
            let key = bs58::encode(&raw[i + 32 * k..i + 32 * (k + 1)]).into_string();
            let writable = if k < req { k < req - ro_signed } else { k < n - ro_unsigned };
            (key, writable)
        })
        .collect()
}

/// A writable, non-fee-payer static key of snippet tx 0.
fn tracked_vault() -> String {
    let raw = &snippet()[0].2;
    static_keys(raw).into_iter().skip(1).find(|(_, w)| *w).expect("writable key").0
}

struct Opts {
    legacy: bool,
    /// Read raw bytes from a synthetic fleet dataset partition instead of the JSONL.
    dataset: Option<PathBuf>,
    max_depth: u32,
    ppm: u32,
    resume: bool,
}

const DEFAULT: Opts = Opts { legacy: false, dataset: None, max_depth: 64, ppm: 1_000_000, resume: false };

fn build_into(f: &Fixture, out: &Path, o: Opts) -> ml_prep::Result<()> {
    build::run(&BuildArgs {
        labels: f.labels.clone(),
        positives: f.index.clone(),
        packs: if o.legacy { f.legacy.clone() } else { f.library.clone() },
        raw: match &o.dataset {
            Some(dir) => RawSource::dataset(dir, &f.labels, 2)?,
            None => RawSource::jsonl(Path::new(SNIPPET))?,
        },
        out: out.to_path_buf(),
        slots_per_part: 1000,
        min_slot: None,
        max_slot: None,
        max_depth: o.max_depth,
        unknown_pair_ppm: o.ppm,
        threads: 2,
        resume: o.resume,
        provenance: vec![],
        dataset_id: "fixture".into(),
        partition_index: None,
        train_routes: None,
    })
}

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ml-prep-test-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn part(out: &Path, dir: &str) -> PathBuf {
    let mut v: Vec<PathBuf> = std::fs::read_dir(out.join(dir))
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|e| e == "parquet"))
        .collect();
    v.sort();
    assert_eq!(v.len(), 1);
    v.pop().unwrap()
}

#[derive(Debug, PartialEq)]
struct TxLabel {
    label: String,
    target: Option<i8>,
    elig_live: u32,
    elig_oracle: u32,
    rank_live: Option<u32>,
    rank_oracle: Option<u32>,
    seen: u32,
}

fn tx_labels(out: &Path) -> BTreeMap<u64, TxLabel> {
    let cols = [
        "master_tx_id", "arb_label", "arb_target", "n_eligible_live", "n_eligible_oracle", "min_cover_rank_live",
        "min_cover_rank_oracle", "n_profitable_routes_seen_in_train",
    ];
    let (reader, order) = pq::open(&part(out, "tx_labels"), &cols).unwrap();
    let mut m = BTreeMap::new();
    for b in reader {
        let b = b.unwrap();
        let c = Cols { batch: &b, order: &order };
        let t = b.column(order[2]).as_any().downcast_ref::<Int8Array>().unwrap();
        let opt = |j: usize, i: usize| {
            let a = c.u32(j).unwrap();
            if a.is_null(i) { None } else { Some(a.value(i)) }
        };
        for i in 0..b.num_rows() {
            m.insert(
                c.u64(0).unwrap().value(i),
                TxLabel {
                    label: c.str(1).unwrap().value(i).to_string(),
                    target: if t.is_null(i) { None } else { Some(t.value(i)) },
                    elig_live: c.u32(3).unwrap().value(i),
                    elig_oracle: c.u32(4).unwrap().value(i),
                    rank_live: opt(5, i),
                    rank_oracle: opt(6, i),
                    seen: c.u32(7).unwrap().value(i),
                },
            );
        }
    }
    m
}

/// (master, pack) -> (target, eligible_live, rank)
fn pairs(out: &Path) -> BTreeMap<(u64, String), (Option<i8>, bool, u32)> {
    let (reader, order) =
        pq::open(&part(out, "pairs"), &["master_tx_id", "pack_id", "pack_target", "eligible_live", "pack_rank"]).unwrap();
    let mut m = BTreeMap::new();
    for b in reader {
        let b = b.unwrap();
        let c = Cols { batch: &b, order: &order };
        let t = b.column(order[2]).as_any().downcast_ref::<Int8Array>().unwrap();
        for i in 0..b.num_rows() {
            m.insert(
                (c.u64(0).unwrap().value(i), c.str(1).unwrap().value(i).to_string()),
                (if t.is_null(i) { None } else { Some(t.value(i)) }, c.bool(3).unwrap().value(i), c.u32(4).unwrap().value(i)),
            );
        }
    }
    m
}

#[test]
fn features_are_the_production_encoder_output() {
    let dir = tmp("parity");
    let f = fixture(&dir, &rows());
    let out = dir.join("out");
    build_into(&f, &out, DEFAULT).unwrap();

    let raw_by_index: BTreeMap<u64, Vec<u8>> = snippet().into_iter().map(|(i, _, r)| (i, r)).collect();
    let mtx_to_index: BTreeMap<u64, u64> = rows().iter().map(|r| (r.0, r.1)).collect();
    let specs = flatten::feature_specs();
    let names: Vec<&str> = specs.iter().map(|s| s.name.as_str()).collect();
    let mut cols = vec!["master_tx_id", "encode_status"];
    cols.extend(names.iter());
    let (reader, order) = pq::open(&part(&out, "tx_features"), &cols).unwrap();
    let mut scratch = Scratch::new();
    let mut enc = EncodedTxV2::boxed();
    let mut expected = vec![0u64; flatten::feature_count()];
    let mut checked = 0;
    for b in reader {
        let b = b.unwrap();
        for i in 0..b.num_rows() {
            let mtx = b.column(order[0]).as_any().downcast_ref::<UInt64Array>().unwrap().value(i);
            let raw = &raw_by_index[&mtx_to_index[&mtx]];
            assert_eq!(encode_transaction_v2(raw, &mut scratch, &mut enc), Status::Ok);
            flatten::flatten_v2(&enc, flatten::compute_budget_hash(), &mut expected);
            for (j, spec) in specs.iter().enumerate() {
                assert_eq!(widen(b.column(order[2 + j]), i), expected[j], "master {mtx} feature {}", spec.name);
            }
            let get = |name: &str| expected[names.iter().position(|n| *n == name).unwrap()];
            assert_eq!(get("raw_len"), enc.base.header.raw_len as u64);
            assert_eq!(get("instruction_count"), enc.base.header.instruction_count as u64);
            assert_eq!(get("requested_cu_price"), enc.base.header.requested_cu_price);
            assert_eq!(get("program_sequence_fingerprint"), enc.base.header.program_sequence_fingerprint);
            assert_eq!(get("config_priority_fee_lamports"), enc.ext.config_priority_fee_lamports);
            checked += 1;
        }
    }
    assert_eq!(checked, 5);
}

fn widen(a: &ArrayRef, i: usize) -> u64 {
    use arrow_array::cast::AsArray;
    use arrow_array::types::*;
    match a.data_type() {
        arrow_schema::DataType::UInt8 => a.as_primitive::<UInt8Type>().value(i) as u64,
        arrow_schema::DataType::UInt16 => a.as_primitive::<UInt16Type>().value(i) as u64,
        arrow_schema::DataType::UInt32 => a.as_primitive::<UInt32Type>().value(i) as u64,
        arrow_schema::DataType::UInt64 => a.as_primitive::<UInt64Type>().value(i),
        other => panic!("unexpected feature type {other:?}"),
    }
}

#[test]
fn unknown_is_never_negative() {
    let dir = tmp("unknown");
    let f = fixture(&dir, &rows());
    let out = dir.join("out");
    build_into(&f, &out, DEFAULT).unwrap();
    let l = tx_labels(&out);
    assert_eq!((l[&1].label.as_str(), l[&1].target), ("POSITIVE", Some(1)));
    assert_eq!((l[&2].label.as_str(), l[&2].target), ("CERTIFIED_NEGATIVE", Some(0)));
    // NEG row + UNKNOWN row with a base: undecided, not negative.
    assert_eq!((l[&3].label.as_str(), l[&3].target), ("UNKNOWN", None));
    assert_eq!((l[&4].label.as_str(), l[&4].target), ("UNKNOWN", None));
    for ((mtx, _), (target, _, _)) in pairs(&out) {
        if l[&mtx].label == "UNKNOWN" {
            assert_eq!(target, None, "pair of UNKNOWN master {mtx} has a target");
        }
    }
}

#[test]
fn pack_targets_follow_exact_route_coverage() {
    let dir = tmp("targets");
    let f = fixture(&dir, &rows());
    let out = dir.join("out");
    build_into(&f, &out, DEFAULT).unwrap();
    let p = pairs(&out);
    // master 1 (POSITIVE, all layers): P1 covers r1; P3 shares the key but not the route; P2's r2 is not profitable.
    assert_eq!(p[&(1, "P1".into())], (Some(1), true, 0));
    assert_eq!(p[&(1, "P3".into())], (Some(0), true, 1));
    assert_eq!(p[&(1, "P2".into())], (Some(0), false, 0));
    // master 5: covered only by the INNER-key pack.
    assert_eq!(p[&(5, "P2".into())], (Some(1), false, 0));
    assert_eq!(p[&(5, "P1".into())], (Some(0), true, 0));
    // CERTIFIED_NEGATIVE: live packs only, all-zero targets.
    assert_eq!(p[&(2, "P1".into())], (Some(0), true, 0));
    assert!(!p.contains_key(&(2, "P2".into())));
    let l = tx_labels(&out);
    assert_eq!((l[&1].elig_live, l[&1].elig_oracle, l[&1].rank_live, l[&1].rank_oracle), (2, 3, Some(0), Some(0)));
    assert_eq!((l[&5].rank_live, l[&5].rank_oracle), (None, Some(0)));
    // NO_LABEL row (base "") still sees live packs for its key.
    assert_eq!(l[&4].elig_live, 2);
    // Route recurrence: library trained on masters 1..=2, so master 1's routes are "seen", master 5's r2 is not.
    assert_eq!((l[&1].seen, l[&5].seen), (2, 0));
}

#[test]
fn depth_limits_pairs_by_catalog_rank() {
    let dir = tmp("depth");
    let f = fixture(&dir, &rows());
    let out = dir.join("out");
    build_into(&f, &out, Opts { max_depth: 1, ..DEFAULT }).unwrap();
    let p = pairs(&out);
    assert!(p.contains_key(&(1, "P1".into())) && !p.contains_key(&(1, "P3".into())));
}

#[test]
fn unknown_pairs_need_relevance_and_sample() {
    let dir = tmp("sample");
    let f = fixture(&dir, &rows());
    // Only snippet tx 0 (master 1, POSITIVE) is relevant, so no UNKNOWN master gets pairs.
    let out = dir.join("all");
    build_into(&f, &out, DEFAULT).unwrap();
    assert!(!pairs(&out).keys().any(|(m, _)| *m == 3 || *m == 4));
    let out = dir.join("none");
    build_into(&f, &out, Opts { ppm: 0, ..DEFAULT }).unwrap();
    assert!(!pairs(&out).keys().any(|(m, _)| *m == 3 || *m == 4));
}

#[test]
fn positives_index_must_match_trigger_labels() {
    let dir = tmp("mismatch");
    let f = fixture(&dir, &rows());
    // Drop one indexed positive occurrence.
    write(
        &f.index.join("positives.parquet"),
        vec![
            ("master_tx_id", u64s(&[1])),
            ("occurrence_index", u64s(&[0])),
            ("program_id", s(&[PROG_A])),
            ("family_id", s(&[FAM_1])),
            ("layer", s(&["TOP_LEVEL"])),
            ("base", s(&[SOL])),
            ("route_ids", s(&["r1,r9"])),
        ],
    );
    let err = build_into(&f, &dir.join("out"), DEFAULT).unwrap_err();
    assert!(err.to_string().contains("indexed positives"), "{err}");
}

#[test]
fn legacy_catalog_uses_the_same_targets() {
    let dir = tmp("legacy");
    let f = fixture(&dir, &rows());
    let (lib, leg) = (dir.join("lib"), dir.join("leg"));
    build_into(&f, &lib, DEFAULT).unwrap();
    build_into(&f, &leg, Opts { legacy: true, ..DEFAULT }).unwrap();
    assert_eq!(pairs(&lib), pairs(&leg));
    assert_eq!(tx_labels(&lib), tx_labels(&leg));
    let m: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(leg.join("manifest.json")).unwrap()).unwrap();
    assert_eq!(m["schemas"]["pack_features"], "packflat-v1");
    let m: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(lib.join("manifest.json")).unwrap()).unwrap();
    assert_eq!(m["schemas"]["pack_features"], "packflat-v2");
    assert_eq!(m["packs"]["train_master_tx_id_max"], TRAIN.1);
}

fn output_hashes(out: &Path) -> Vec<(String, String)> {
    let m: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(out.join("manifest.json")).unwrap()).unwrap();
    m["outputs"]
        .as_array()
        .unwrap()
        .iter()
        .map(|o| (o["path"].as_str().unwrap().to_string(), o["sha256"].as_str().unwrap().to_string()))
        .collect()
}

#[test]
fn builds_are_deterministic_and_resumable() {
    let dir = tmp("determinism");
    let f = fixture(&dir, &rows());
    let (a, b) = (dir.join("a"), dir.join("b"));
    build_into(&f, &a, DEFAULT).unwrap();
    build_into(&f, &b, DEFAULT).unwrap();
    let ha = output_hashes(&a);
    assert!(!ha.is_empty());
    assert_eq!(ha, output_hashes(&b));
    for (rel, _) in &ha {
        assert_eq!(std::fs::read(a.join(rel)).unwrap(), std::fs::read(b.join(rel)).unwrap(), "{rel}");
    }
    assert_eq!(std::fs::read(a.join("manifest.json")).unwrap(), std::fs::read(b.join("manifest.json")).unwrap());

    // A second build into a used dir needs --resume.
    assert!(build_into(&f, &a, DEFAULT).is_err());
    // Drop finished features and pairs parts; resume rebuilds them identically.
    std::fs::remove_file(part(&a, "tx_features")).unwrap();
    std::fs::remove_file(part(&a, "pairs")).unwrap();
    build_into(&f, &a, Opts { resume: true, ..DEFAULT }).unwrap();
    assert_eq!(output_hashes(&a), ha);
}

#[test]
fn feature_files_hold_no_labels_or_identifiers() {
    let dir = tmp("schema");
    let f = fixture(&dir, &rows());
    let out = dir.join("out");
    build_into(&f, &out, DEFAULT).unwrap();
    let file = File::open(part(&out, "tx_features")).unwrap();
    let builder = parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder::try_new(file).unwrap();
    let fm: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(out.join("feature_manifest.json")).unwrap()).unwrap();
    let banned: Vec<&str> = ["id_columns", "label_columns", "meta_columns", "gate_columns"]
        .iter()
        .flat_map(|k| fm[k].as_array().unwrap().iter().map(|v| v.as_str().unwrap()))
        .collect();
    let feature_names: Vec<&str> = fm["tx_features"].as_array().unwrap().iter().map(|v| v["name"].as_str().unwrap()).collect();
    for field in builder.schema().fields() {
        let role = field.metadata().get("role").map(String::as_str).unwrap_or("");
        assert!(matches!(role, "id" | "meta" | "feature" | "gate"), "{} has role {role}", field.name());
        if role == "feature" {
            assert!(!banned.contains(&field.name().as_str()), "{}", field.name());
            assert!(feature_names.contains(&field.name().as_str()));
        } else {
            assert!(!feature_names.contains(&field.name().as_str()));
        }
        assert!(!field.name().contains("label") && !field.name().contains("target"), "{}", field.name());
    }
    let file_features: Vec<String> = builder
        .schema()
        .fields()
        .iter()
        .filter(|f| f.metadata().get("role").map(String::as_str) == Some("feature"))
        .map(|f| f.name().clone())
        .collect();
    assert_eq!(file_features, feature_names);
    // Pack features: only static structure plus the library's training-window metadata.
    for p in fm["pack_features"].as_array().unwrap() {
        let name = p["name"].as_str().unwrap();
        assert!(matches!(p["group"].as_str().unwrap(), "static" | "train_meta"), "{name}");
        for banned in ["ppm", "objective", "positive_cov", "value_cov", "target", "label"] {
            assert!(!name.contains(banned), "{name}");
        }
    }
}

#[test]
fn relevance_uses_only_static_wire_keys() {
    let dir = tmp("relevance");
    let f = fixture(&dir, &rows());
    let out = dir.join("out");
    build_into(&f, &out, DEFAULT).unwrap();
    let vault = tracked_vault();
    let raw_by_index: BTreeMap<u64, Vec<u8>> = snippet().into_iter().map(|(i, _, r)| (i, r)).collect();
    let mtx_to_index: BTreeMap<u64, u64> = rows().iter().map(|r| (r.0, r.1)).collect();
    let (reader, order) = pq::open(
        &part(&out, "tx_features"),
        &["master_tx_id", "rel_market_writable", "rel_market_readonly", "rel_venue_program", "relevant"],
    )
    .unwrap();
    let mut relevant_rows = 0;
    for b in reader {
        let b = b.unwrap();
        let get16 = |j: usize, i: usize| {
            b.column(order[j]).as_any().downcast_ref::<arrow_array::UInt16Array>().unwrap().value(i)
        };
        for i in 0..b.num_rows() {
            let mtx = b.column(order[0]).as_any().downcast_ref::<UInt64Array>().unwrap().value(i);
            let keys = static_keys(&raw_by_index[&mtx_to_index[&mtx]]);
            let w = keys.iter().filter(|(k, wr)| *k == vault && *wr).count() as u16;
            let r = keys.iter().filter(|(k, wr)| *k == vault && !*wr).count() as u16;
            let p = keys.iter().filter(|(k, _)| k == PROG_A).count() as u16;
            assert_eq!((get16(1, i), get16(2, i), get16(3, i)), (w, r, p), "master {mtx}");
            let rel = b.column(order[4]).as_any().downcast_ref::<UInt8Array>().unwrap().value(i);
            assert_eq!(rel == 1, w + r + p > 0, "master {mtx}");
            relevant_rows += rel as u32;
        }
    }
    assert!(relevant_rows >= 1, "the fixture vault must make tx 0 relevant");
    let m: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(out.join("manifest.json")).unwrap()).unwrap();
    assert_eq!(m["relevance"]["market_accounts"], 2); // vault + pool; the shared mint is excluded
}

/// A fleet dataset partition holding the snippet transactions as CBOR records
/// `{"slot": .., "signature": .., "raw": bytes, "instructions": [..]}`.
fn dataset_fixture(dir: &Path) -> PathBuf {
    let ds = dir.join("dataset");
    std::fs::create_dir_all(&ds).unwrap();
    std::fs::write(
        ds.join("compaction.json"),
        format!(r#"{{"partitions":[{{"part":0,"first_slot":{SLOT},"last_slot":{SLOT}}}]}}"#),
    )
    .unwrap();
    let txs = snippet();
    let cbor_text = |s: &str| {
        let mut v = vec![0x78, s.len() as u8];
        v.extend(s.as_bytes());
        v
    };
    let records: Vec<Vec<u8>> = txs
        .iter()
        .map(|(_, sig, raw)| {
            let mut v = vec![0xa4, 0x64, b's', b'l', b'o', b't', 0x1a];
            v.extend((SLOT as u32).to_be_bytes());
            v.extend([0x69]);
            v.extend(b"signature");
            v.extend(cbor_text(sig));
            v.extend([0x6c]);
            v.extend(b"instructions");
            v.extend([0x82, 0xa1, 0x61, b'd', 0x42, 0xde, 0xad, 0xf6]);
            v.extend([0x63, b'r', b'a', b'w', 0x59]);
            v.extend((raw.len() as u16).to_be_bytes());
            v.extend(raw);
            v
        })
        .collect();
    write(
        &ds.join("part-000000.transactions.parquet"),
        vec![
            ("slot", u64s(&vec![SLOT; txs.len()])),
            ("entry_index", u64s(&txs.iter().map(|(i, _, _)| i + 3).collect::<Vec<_>>())),
            ("tx_index", u64s(&txs.iter().map(|(i, _, _)| *i).collect::<Vec<_>>())),
            ("signature", Arc::new(StringArray::from_iter_values(txs.iter().map(|(_, s, _)| s.clone())))),
            ("record_cbor", Arc::new(arrow_array::BinaryArray::from_iter_values(records.iter()))),
        ],
    );
    ds
}

#[test]
fn dataset_partition_source_matches_jsonl() {
    let dir = tmp("dataset");
    let f = fixture(&dir, &rows());
    let ds = dataset_fixture(&dir);
    let (a, b) = (dir.join("jsonl"), dir.join("dataset-out"));
    build_into(&f, &a, DEFAULT).unwrap();
    build_into(&f, &b, Opts { dataset: Some(ds), ..DEFAULT }).unwrap();
    for sub in ["tx_features", "pairs", "tx_labels"] {
        assert_eq!(std::fs::read(part(&a, sub)).unwrap(), std::fs::read(part(&b, sub)).unwrap(), "{sub}");
    }
    let m: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(b.join("manifest.json")).unwrap()).unwrap();
    assert_eq!(m["raw_source"]["kind"], "fleet-dataset-transactions-parquet");
}
