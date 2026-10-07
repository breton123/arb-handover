//! Labels pass: one streaming walk over `trigger_occurrences` (sorted by
//! `master_tx_id`), joined to the candidate-library positives index.
//!
//! Memory is one transaction's trigger rows, one output part, and the positives
//! index (POSITIVE occurrences only). Writes `tx_labels/part-*.parquet` (one row
//! per master transaction) and `keys.parquet` (the trigger-key dictionary).
//!
//! Transaction label:
//! - `POSITIVE` when any trigger row is POSITIVE (`arb_target = 1`)
//! - `CERTIFIED_NEGATIVE` when there are CERTIFIED_NEGATIVE rows, no POSITIVE
//!   row, and no UNKNOWN row with a base. An UNKNOWN row with a base means
//!   another base could not be decided (`arb_target = 0`)
//! - otherwise `UNKNOWN` (`arb_target = null`). UNKNOWN never becomes 0.
//!
//! Profitable RouteIds come from the index's `positives.parquet`
//! (`candidate-packs index-library`), the same source `build-library` and
//! `sweep-library` read. A pack covers a transaction when its RouteIds intersect
//! the transaction's profitable RouteIds.

use std::collections::{BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow_array::builder::{ListBuilder, UInt32Builder};
use arrow_array::{ArrayRef, Int64Array, Int8Array, RecordBatch, StringArray, UInt32Array, UInt64Array, UInt8Array};
use arrow_schema::{DataType, Field, Schema};
use parquet::arrow::arrow_reader::ParquetRecordBatchReader;
use serde::{Deserialize, Serialize};
use tx_features::mix64;

use crate::packs::PackIndex;
use crate::pq::{self, Cols};
use crate::Result;

pub const TX_LABELS_SCHEMA: &str = "ml-tx-labels-2";
pub const INDEX_SCHEMA: &str = "candidate-library-index-v1";
/// Seed of the deterministic UNKNOWN-pair sample.
const SAMPLE_SEED: u64 = 0x5EED_0F_7A1B_2C3D;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Status {
    Unknown,
    Positive,
    Negative,
}

impl Status {
    fn parse(s: &str) -> Result<Self> {
        match s {
            "POSITIVE" => Ok(Status::Positive),
            "CERTIFIED_NEGATIVE" => Ok(Status::Negative),
            "UNKNOWN" => Ok(Status::Unknown),
            other => Err(format!("unknown label_status {other}").into()),
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Status::Positive => "POSITIVE",
            Status::Negative => "CERTIFIED_NEGATIVE",
            Status::Unknown => "UNKNOWN",
        }
    }
    pub fn target(self) -> Option<i8> {
        match self {
            Status::Positive => Some(1),
            Status::Negative => Some(0),
            Status::Unknown => None,
        }
    }
}

/// Deterministic per-transaction sample: `true` for about `ppm / 1e6` of master ids.
pub fn sampled(mtx: u64, ppm: u32) -> bool {
    mix64(SAMPLE_SEED, mtx) % 1_000_000 < ppm as u64
}

struct TrigRow {
    mtx: u64,
    signature: String,
    slot: u64,
    entry_index: u64,
    tx_index: u64,
    base: String,
    program: String,
    family: String,
    layer: String,
    status: Status,
    unknown_reason: String,
    best_net_profit: Option<i64>,
    quality: String,
}

const TRIG_COLS: &[&str] = &[
    "master_tx_id", "signature", "slot", "entry_index", "tx_index", "base", "program_id", "family_id",
    "layer", "label_status", "unknown_reason", "best_net_profit", "label_quality",
];

fn trig_rows(c: &Cols, n: usize) -> Result<Vec<TrigRow>> {
    let (mtx, sig, slot, entry, txi) = (c.u64(0)?, c.str(1)?, c.u64(2)?, c.u64(3)?, c.u64(4)?);
    let (base, prog, fam, layer) = (c.str(5)?, c.str(6)?, c.str(7)?, c.str(8)?);
    let (status, reason, best, quality) = (c.str(9)?, c.str(10)?, c.i64(11)?, c.str(12)?);
    (0..n)
        .map(|i| {
            Ok(TrigRow {
                mtx: mtx.value(i),
                signature: sig.value(i).to_string(),
                slot: slot.value(i),
                entry_index: entry.value(i),
                tx_index: txi.value(i),
                base: base.value(i).to_string(),
                program: prog.value(i).to_string(),
                family: fam.value(i).to_string(),
                layer: layer.value(i).to_string(),
                status: Status::parse(status.value(i))?,
                unknown_reason: reason.value(i).to_string(),
                best_net_profit: pq::opt_i64(best, i),
                quality: quality.value(i).to_string(),
            })
        })
        .collect()
}

/// Consecutive rows sharing `master_tx_id`. Errors if the file is not sorted by it.
struct Groups<T> {
    reader: ParquetRecordBatchReader,
    order: Vec<usize>,
    rows: std::vec::IntoIter<T>,
    pending: Option<T>,
    last: Option<u64>,
    extract: fn(&Cols, usize) -> Result<Vec<T>>,
    key: fn(&T) -> u64,
    name: String,
}

impl<T> Groups<T> {
    fn new(path: &Path, cols: &[&str], extract: fn(&Cols, usize) -> Result<Vec<T>>, key: fn(&T) -> u64) -> Result<Self> {
        let (reader, order) = pq::open(path, cols)?;
        Ok(Self {
            reader,
            order,
            rows: Vec::new().into_iter(),
            pending: None,
            last: None,
            extract,
            key,
            name: path.display().to_string(),
        })
    }

    fn next_row(&mut self) -> Result<Option<T>> {
        loop {
            if let Some(r) = self.rows.next() {
                return Ok(Some(r));
            }
            match self.reader.next() {
                None => return Ok(None),
                Some(batch) => {
                    let batch: RecordBatch = batch?;
                    let c = Cols { batch: &batch, order: &self.order };
                    self.rows = (self.extract)(&c, batch.num_rows())?.into_iter();
                }
            }
        }
    }

    fn next_group(&mut self) -> Result<Option<(u64, Vec<T>)>> {
        let first = match self.pending.take() {
            Some(r) => r,
            None => match self.next_row()? {
                Some(r) => r,
                None => return Ok(None),
            },
        };
        let k = (self.key)(&first);
        if let Some(last) = self.last {
            if k <= last {
                return Err(format!("{} is not sorted by master_tx_id ({k} after {last})", self.name).into());
            }
        }
        self.last = Some(k);
        let mut group = vec![first];
        while let Some(r) = self.next_row()? {
            if (self.key)(&r) == k {
                group.push(r);
            } else {
                self.pending = Some(r);
                break;
            }
        }
        Ok(Some((k, group)))
    }
}

/// POSITIVE occurrences of one partition with their profitable RouteIds.
pub struct Positives {
    /// master_tx_id -> one entry per positive occurrence: comma-joined RouteIds.
    by_tx: HashMap<u64, Vec<String>>,
    pub occurrences: u64,
}

impl Positives {
    pub fn load(index_dir: &Path) -> Result<Self> {
        let manifest: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(index_dir.join("library-index.json"))?)?;
        if manifest["schema"] != INDEX_SCHEMA {
            return Err(format!("{}: unknown index schema {}", index_dir.display(), manifest["schema"]).into());
        }
        let (reader, order) = pq::open(&index_dir.join("positives.parquet"), &["master_tx_id", "route_ids"])?;
        let mut by_tx: HashMap<u64, Vec<String>> = HashMap::new();
        let mut occurrences = 0;
        for batch in reader {
            let batch = batch?;
            let c = Cols { batch: &batch, order: &order };
            for i in 0..batch.num_rows() {
                by_tx.entry(c.u64(0)?.value(i)).or_default().push(c.str(1)?.value(i).to_string());
                occurrences += 1;
            }
        }
        Ok(Self { by_tx, occurrences })
    }

    fn routes_in(&self, window: (u64, u64)) -> HashSet<&str> {
        self.by_tx
            .iter()
            .filter(|(m, _)| (window.0..=window.1).contains(*m))
            .flat_map(|(_, v)| v.iter().flat_map(|s| s.split(',')).filter(|x| !x.is_empty()))
            .collect()
    }
}

#[derive(Default)]
struct TxLabelCols {
    mtx: Vec<u64>,
    slot: Vec<u64>,
    entry: Vec<u64>,
    tx_index: Vec<u64>,
    signature: Vec<String>,
    label: Vec<&'static str>,
    target: Vec<Option<i8>>,
    quality: Vec<String>,
    unknown_reasons: Vec<String>,
    n_occ: Vec<u32>,
    n_top: Vec<u32>,
    n_inner: Vec<u32>,
    n_attr: Vec<u32>,
    keys: Vec<Vec<u32>>,
    n_elig_live: Vec<u32>,
    n_elig_oracle: Vec<u32>,
    n_cov_live: Vec<u32>,
    n_cov_oracle: Vec<u32>,
    rank_live: Vec<Option<u32>>,
    rank_oracle: Vec<Option<u32>>,
    n_profitable: Vec<u32>,
    n_seen: Vec<u32>,
    profitable: Vec<String>,
    best: Vec<Option<i64>>,
    pair_sample: Vec<u8>,
}

pub fn tx_labels_schema() -> Arc<Schema> {
    use pq::field;
    let list = DataType::List(Arc::new(Field::new("item", DataType::UInt32, true)));
    Arc::new(Schema::new_with_metadata(
        vec![
            field("master_tx_id", DataType::UInt64, false, "id", &[]),
            field("slot", DataType::UInt64, false, "id", &[]),
            field("entry_index", DataType::UInt64, false, "id", &[]),
            field("tx_index", DataType::UInt64, false, "id", &[]),
            field("signature", DataType::Utf8, false, "id", &[]),
            field("arb_label", DataType::Utf8, false, "label", &[]),
            field("arb_target", DataType::Int8, true, "label", &[]),
            field("label_quality", DataType::Utf8, false, "label", &[]),
            field("unknown_reasons", DataType::Utf8, false, "label", &[]),
            // Occurrence counts and keys include INNER/ATTRIBUTED rows, which come
            // from execution metadata. Not features.
            field("n_occurrences", DataType::UInt32, false, "meta", &[]),
            field("n_occ_top_level", DataType::UInt32, false, "meta", &[]),
            field("n_occ_inner", DataType::UInt32, false, "meta", &[]),
            field("n_occ_attributed", DataType::UInt32, false, "meta", &[]),
            field("trigger_keys", list, false, "meta", &[]),
            field("n_eligible_live", DataType::UInt32, false, "meta", &[]),
            field("n_eligible_oracle", DataType::UInt32, false, "meta", &[]),
            field("n_covering_live", DataType::UInt32, false, "label", &[]),
            field("n_covering_oracle", DataType::UInt32, false, "label", &[]),
            // Lowest catalog rank of a covering pack over all keys: covered at depth k iff < k.
            field("min_cover_rank_live", DataType::UInt32, true, "label", &[]),
            field("min_cover_rank_oracle", DataType::UInt32, true, "label", &[]),
            field("n_profitable_routes", DataType::UInt32, false, "label", &[]),
            field("n_profitable_routes_seen_in_train", DataType::UInt32, false, "label", &[]),
            field("profitable_routes", DataType::Utf8, false, "label", &[]),
            field("best_net_profit", DataType::Int64, true, "label", &[]),
            field("pair_sample", DataType::UInt8, false, "meta", &[]),
        ],
        [("schema".to_string(), TX_LABELS_SCHEMA.to_string())].into(),
    ))
}

impl TxLabelCols {
    fn columns(&mut self) -> Vec<ArrayRef> {
        let t = std::mem::take(self);
        let mut keys = ListBuilder::new(UInt32Builder::new());
        for k in &t.keys {
            keys.values().append_slice(k);
            keys.append(true);
        }
        vec![
            Arc::new(UInt64Array::from(t.mtx)),
            Arc::new(UInt64Array::from(t.slot)),
            Arc::new(UInt64Array::from(t.entry)),
            Arc::new(UInt64Array::from(t.tx_index)),
            Arc::new(StringArray::from(t.signature)),
            Arc::new(StringArray::from(t.label)),
            Arc::new(Int8Array::from(t.target)),
            Arc::new(StringArray::from(t.quality)),
            Arc::new(StringArray::from(t.unknown_reasons)),
            Arc::new(UInt32Array::from(t.n_occ)),
            Arc::new(UInt32Array::from(t.n_top)),
            Arc::new(UInt32Array::from(t.n_inner)),
            Arc::new(UInt32Array::from(t.n_attr)),
            Arc::new(keys.finish()),
            Arc::new(UInt32Array::from(t.n_elig_live)),
            Arc::new(UInt32Array::from(t.n_elig_oracle)),
            Arc::new(UInt32Array::from(t.n_cov_live)),
            Arc::new(UInt32Array::from(t.n_cov_oracle)),
            Arc::new(UInt32Array::from(t.rank_live)),
            Arc::new(UInt32Array::from(t.rank_oracle)),
            Arc::new(UInt32Array::from(t.n_profitable)),
            Arc::new(UInt32Array::from(t.n_seen)),
            Arc::new(StringArray::from(t.profitable)),
            Arc::new(Int64Array::from(t.best)),
            Arc::new(UInt8Array::from(t.pair_sample)),
        ]
    }
}

/// Trigger-key dictionary: `trigger_keys` ordinals index into this.
pub type Key = (String, String, String, String);

pub fn keys_schema() -> Arc<Schema> {
    use pq::field;
    Arc::new(Schema::new(vec![
        field("key", DataType::UInt32, false, "id", &[]),
        field("program_id", DataType::Utf8, false, "meta", &[]),
        field("family_id", DataType::Utf8, false, "meta", &[]),
        field("layer", DataType::Utf8, false, "meta", &[]),
        field("base", DataType::Utf8, false, "meta", &[]),
    ]))
}

pub fn read_keys(path: &Path) -> Result<Vec<Key>> {
    let (reader, order) = pq::open(path, &["key", "program_id", "family_id", "layer", "base"])?;
    let mut keys = Vec::new();
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        for i in 0..batch.num_rows() {
            if c.u32(0)?.value(i) as usize != keys.len() {
                return Err("keys.parquet is not dense".into());
            }
            keys.push((
                c.str(1)?.value(i).to_string(),
                c.str(2)?.value(i).to_string(),
                c.str(3)?.value(i).to_string(),
                c.str(4)?.value(i).to_string(),
            ));
        }
    }
    Ok(keys)
}

#[derive(Default, Serialize, Clone, Deserialize)]
pub struct LabelSummary {
    pub transactions: u64,
    pub tx_positive: u64,
    pub tx_certified_negative: u64,
    pub tx_unknown: u64,
    pub trigger_rows: u64,
    pub trigger_rows_positive: u64,
    pub trigger_rows_certified_negative: u64,
    pub trigger_rows_unknown: u64,
    pub trigger_keys: u64,
    pub positive_occurrences_indexed: u64,
    pub positive_tx_with_covering_pack_oracle: u64,
    pub positive_tx_with_covering_pack_live: u64,
    pub positive_tx_with_eligible_pack_oracle: u64,
    pub positive_tx_with_eligible_pack_live: u64,
    pub positive_tx_with_route_seen_in_train: u64,
    pub unknown_pair_sample: u64,
    pub slot_min: Option<u64>,
    pub slot_max: Option<u64>,
    pub parts: Vec<String>,
}

pub struct LabelsArgs<'a> {
    pub trigger_occurrences: &'a Path,
    pub positives: &'a Positives,
    pub out: &'a Path,
    pub slots_per_part: u64,
    pub min_slot: Option<u64>,
    pub max_slot: Option<u64>,
    pub unknown_pair_ppm: u32,
    /// RouteIds profitable anywhere in the library's training window, across
    /// partitions (`ml-prep library-support`). Without it, this partition's own
    /// positives inside the pack training window are used.
    pub train_routes: Option<&'a HashSet<String>>,
}

pub fn part_name(part: u64) -> String {
    format!("part-{part:08}.parquet")
}

pub fn run(args: &LabelsArgs, packs: &PackIndex) -> Result<LabelSummary> {
    let train_routes: HashSet<&str> = match (args.train_routes, packs.train_window) {
        (Some(set), _) => set.iter().map(String::as_str).collect(),
        (None, Some(w)) => args.positives.routes_in(w),
        (None, None) => HashSet::new(),
    };
    let mut trig = Groups::new(args.trigger_occurrences, TRIG_COLS, trig_rows, |r: &TrigRow| r.mtx)?;
    let tx_dir: PathBuf = args.out.join("tx_labels");
    std::fs::create_dir_all(&tx_dir)?;

    let mut s = LabelSummary { positive_occurrences_indexed: args.positives.occurrences, ..Default::default() };
    let mut txc = TxLabelCols::default();
    let mut key_ids: HashMap<Key, u32> = HashMap::new();
    let mut key_list: Vec<Key> = Vec::new();
    let mut seen_positive_occurrences = 0u64;
    let mut current_part: Option<u64> = None;
    let flush = |part: u64, txc: &mut TxLabelCols, s: &mut LabelSummary| -> Result<()> {
        let name = part_name(part);
        pq::write_file(&tx_dir.join(&name), tx_labels_schema(), txc.columns())?;
        s.parts.push(name);
        Ok(())
    };

    while let Some((mtx, rows)) = trig.next_group()? {
        let first = &rows[0];
        for r in &rows {
            if r.signature != first.signature || r.slot != first.slot || r.tx_index != first.tx_index || r.entry_index != first.entry_index {
                return Err(format!("master_tx_id {mtx} has inconsistent identity columns").into());
            }
        }
        let slot = first.slot;
        let n_pos = rows.iter().filter(|r| r.status == Status::Positive).count();
        let indexed = args.positives.by_tx.get(&mtx).map(Vec::as_slice).unwrap_or(&[]);
        if indexed.len() != n_pos {
            return Err(format!("master_tx_id {mtx}: {n_pos} POSITIVE trigger rows but {} indexed positives", indexed.len()).into());
        }
        seen_positive_occurrences += n_pos as u64;
        if args.min_slot.is_some_and(|m| slot < m) || args.max_slot.is_some_and(|m| slot > m) {
            continue;
        }
        let part = slot / args.slots_per_part;
        match current_part {
            Some(p) if p == part => {}
            Some(p) if p > part => return Err(format!("slot order went backwards at master_tx_id {mtx}").into()),
            Some(p) => {
                flush(p, &mut txc, &mut s)?;
                current_part = Some(part);
            }
            None => current_part = Some(part),
        }
        s.slot_min = Some(s.slot_min.map_or(slot, |m| m.min(slot)));
        s.slot_max = Some(s.slot_max.map_or(slot, |m| m.max(slot)));

        let n_neg = rows.iter().filter(|r| r.status == Status::Negative).count();
        let n_unk = rows.len() - n_pos - n_neg;
        let unknown_with_base = rows.iter().any(|r| r.status == Status::Unknown && !r.base.is_empty());
        let label = if n_pos > 0 {
            Status::Positive
        } else if n_neg > 0 && !unknown_with_base {
            Status::Negative
        } else {
            Status::Unknown
        };
        s.trigger_rows += rows.len() as u64;
        s.trigger_rows_positive += n_pos as u64;
        s.trigger_rows_certified_negative += n_neg as u64;
        s.trigger_rows_unknown += n_unk as u64;

        // Distinct trigger keys (interned in first-seen order: deterministic).
        let mut keys: BTreeSet<u32> = BTreeSet::new();
        for r in &rows {
            let k: Key = (r.program.clone(), r.family.clone(), r.layer.clone(), r.base.clone());
            let id = *key_ids.entry(k.clone()).or_insert_with(|| {
                key_list.push(k);
                (key_list.len() - 1) as u32
            });
            keys.insert(id);
        }
        let mut eligible: BTreeSet<usize> = BTreeSet::new();
        for &k in &keys {
            let (p, f, l, b) = &key_list[k as usize];
            eligible.extend(packs.eligible(p, f, l, b).iter().copied());
        }

        let profitable: BTreeSet<&str> =
            indexed.iter().flat_map(|s| s.split(',')).filter(|x| !x.is_empty()).collect();
        if label != Status::Positive && !profitable.is_empty() {
            return Err(format!("master_tx_id {mtx}: profitable routes on a non-positive transaction").into());
        }
        let covers = |p: usize| packs.packs[p].route_ids.iter().any(|r| profitable.contains(r.as_str()));
        let covering: Vec<usize> = eligible.iter().copied().filter(|&p| covers(p)).collect();
        let live = |p: &usize| packs.is_live(*p);
        let n_elig_live = eligible.iter().filter(|p| live(p)).count() as u32;
        let n_cov_live = covering.iter().filter(|p| live(p)).count() as u32;
        let rank_oracle = covering.iter().map(|&p| packs.packs[p].rank).min();
        let rank_live = covering.iter().filter(|p| live(p)).map(|&p| packs.packs[p].rank).min();
        let n_seen = profitable.iter().filter(|r| train_routes.contains(*r)).count() as u32;

        s.transactions += 1;
        match label {
            Status::Positive => {
                s.tx_positive += 1;
                s.positive_tx_with_eligible_pack_oracle += (!eligible.is_empty()) as u64;
                s.positive_tx_with_eligible_pack_live += (n_elig_live > 0) as u64;
                s.positive_tx_with_covering_pack_oracle += (!covering.is_empty()) as u64;
                s.positive_tx_with_covering_pack_live += (n_cov_live > 0) as u64;
                s.positive_tx_with_route_seen_in_train += (n_seen > 0) as u64;
            }
            Status::Negative => s.tx_certified_negative += 1,
            Status::Unknown => s.tx_unknown += 1,
        }
        let sample = label == Status::Unknown && sampled(mtx, args.unknown_pair_ppm);
        s.unknown_pair_sample += sample as u64;

        let mut reasons: Vec<&str> =
            rows.iter().filter(|r| r.status == Status::Unknown).map(|r| r.unknown_reason.as_str()).collect();
        reasons.sort_unstable();
        reasons.dedup();
        let mut qualities: Vec<&str> = rows.iter().map(|r| r.quality.as_str()).filter(|q| !q.is_empty()).collect();
        qualities.sort_unstable();
        qualities.dedup();
        let count_layer = |l: &str| rows.iter().filter(|r| r.layer == l).count() as u32;

        txc.mtx.push(mtx);
        txc.slot.push(slot);
        txc.entry.push(first.entry_index);
        txc.tx_index.push(first.tx_index);
        txc.signature.push(first.signature.clone());
        txc.label.push(label.name());
        txc.target.push(label.target());
        txc.quality.push(qualities.join("|"));
        txc.unknown_reasons.push(reasons.join("|"));
        txc.n_occ.push(rows.len() as u32);
        txc.n_top.push(count_layer("TOP_LEVEL"));
        txc.n_inner.push(count_layer("INNER"));
        txc.n_attr.push(count_layer("ATTRIBUTED"));
        txc.keys.push(keys.into_iter().collect());
        txc.n_elig_live.push(n_elig_live);
        txc.n_elig_oracle.push(eligible.len() as u32);
        txc.n_cov_live.push(n_cov_live);
        txc.n_cov_oracle.push(covering.len() as u32);
        txc.rank_live.push(rank_live);
        txc.rank_oracle.push(rank_oracle);
        txc.n_profitable.push(profitable.len() as u32);
        txc.n_seen.push(n_seen);
        txc.profitable.push(profitable.iter().copied().collect::<Vec<_>>().join(","));
        txc.best.push(rows.iter().filter_map(|r| r.best_net_profit).max());
        txc.pair_sample.push(sample as u8);
    }
    if let Some(p) = current_part {
        flush(p, &mut txc, &mut s)?;
    }
    if seen_positive_occurrences != args.positives.occurrences {
        return Err(format!(
            "positives index has {} occurrences, trigger_occurrences has {} POSITIVE rows",
            args.positives.occurrences, seen_positive_occurrences
        )
        .into());
    }
    s.trigger_keys = key_list.len() as u64;
    let k = &key_list;
    let cols: Vec<ArrayRef> = vec![
        Arc::new(UInt32Array::from_iter_values(0..k.len() as u32)),
        Arc::new(StringArray::from_iter_values(k.iter().map(|x| x.0.as_str()))),
        Arc::new(StringArray::from_iter_values(k.iter().map(|x| x.1.as_str()))),
        Arc::new(StringArray::from_iter_values(k.iter().map(|x| x.2.as_str()))),
        Arc::new(StringArray::from_iter_values(k.iter().map(|x| x.3.as_str()))),
    ];
    pq::write_file(&args.out.join("keys.parquet"), keys_schema(), cols)?;
    Ok(s)
}
