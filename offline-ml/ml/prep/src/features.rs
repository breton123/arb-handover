//! Features pass: raw transaction -> `encode_transaction_v2` (framed-tx-v2) -> `txflat-v2`.
//!
//! One output part per `tx_labels` part, built independently (in parallel).
//! The part has the same rows as its `tx_labels` part, in the same
//! order, and contains no label column.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;

use arrow_array::{ArrayRef, Int32Array, Int64Array, UInt16Array, UInt64Array, UInt8Array};
use arrow_schema::{DataType, Schema};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tx_features::{encode_transaction_v2, EncodedTxV2, Scratch, Status, FEATURE_SCHEMA_V2};

use crate::flatten::{self, FLAT_SCHEMA};
use crate::pq::{self, Cols};
use crate::raw::{first_signature_b58, is_v1_format, RawSource};
use crate::relevance::{TrackedUniverse, RELEVANCE_SCHEMA};
use crate::Result;

#[derive(Serialize, Deserialize, Clone, Default)]
pub struct PartReport {
    pub part: String,
    pub rows: u64,
    pub encode_ok: u64,
    pub encode_failed: u64,
    /// Transactions in the v1 wire format (0x81 prefix). framed-tx-v2 encodes them.
    #[serde(default)]
    pub v1_format: u64,
    /// Rows the relevance gate passes.
    pub relevant: u64,
    pub slots: u64,
    /// sha256 over the per-slot source sha256s, in slot order.
    pub raw_digest: String,
    /// Static program pubkeys seen, keyed by `identity_hash64` (hex).
    pub programs: BTreeMap<String, String>,
}

pub fn schema() -> Arc<Schema> {
    use pq::field;
    let mut fields = vec![
        field("master_tx_id", DataType::UInt64, false, "id", &[]),
        field("slot", DataType::UInt64, false, "id", &[]),
        field("block_time", DataType::Int64, true, "meta", &[]),
        field("encode_status", DataType::Int32, false, "meta", &[]),
    ];
    // Relevance gate: raw-only, deterministic, applied before the model. Not model features.
    for (name, dtype) in GATE_COLUMNS {
        fields.push(field(name, dtype.clone(), false, "gate", &[("schema", RELEVANCE_SCHEMA)]));
    }
    for s in flatten::feature_specs() {
        fields.push(field(
            &s.name,
            pq::width_type(s.width),
            false,
            "feature",
            &[("group", s.group), ("kind", s.kind.name()), ("source_width", &s.width.to_string())],
        ));
    }
    Arc::new(Schema::new_with_metadata(
        fields,
        [
            ("feature_schema".to_string(), FLAT_SCHEMA.to_string()),
            ("encoder_schema".to_string(), FEATURE_SCHEMA_V2.to_string()),
        ]
        .into(),
    ))
}

pub const GATE_COLUMNS: [(&str, DataType); 4] = [
    ("rel_market_writable", DataType::UInt16),
    ("rel_market_readonly", DataType::UInt16),
    ("rel_venue_program", DataType::UInt16),
    ("relevant", DataType::UInt8),
];

struct MasterRow {
    mtx: u64,
    slot: u64,
    entry_index: u64,
    tx_index: u64,
    signature: String,
}

fn read_master(path: &Path) -> Result<Vec<MasterRow>> {
    let (reader, order) = pq::open(path, &["master_tx_id", "slot", "entry_index", "tx_index", "signature"])?;
    let mut out = Vec::new();
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        let (m, s, e, t, sig) = (c.u64(0)?, c.u64(1)?, c.u64(2)?, c.u64(3)?, c.str(4)?);
        for i in 0..batch.num_rows() {
            out.push(MasterRow {
                mtx: m.value(i),
                slot: s.value(i),
                entry_index: e.value(i),
                tx_index: t.value(i),
                signature: sig.value(i).to_string(),
            });
        }
    }
    Ok(out)
}

pub fn build_part(labels_part: &Path, out_part: &Path, source: &RawSource, universe: &TrackedUniverse) -> Result<PartReport> {
    let name = labels_part.file_name().unwrap().to_string_lossy().to_string();
    let master = read_master(labels_part)?;
    let width = flatten::feature_count();
    let cb_hash = flatten::compute_budget_hash();
    let mut scratch = Scratch::new();
    let mut enc = EncodedTxV2::boxed();
    let mut row = vec![0u64; width];

    let n = master.len();
    let mut mtx = Vec::with_capacity(n);
    let mut slots = Vec::with_capacity(n);
    let mut block_time = Vec::with_capacity(n);
    let mut status_col = Vec::with_capacity(n);
    let mut gate: [Vec<u16>; 3] = [Vec::with_capacity(n), Vec::with_capacity(n), Vec::with_capacity(n)];
    let mut relevant = Vec::with_capacity(n);
    let mut values: Vec<Vec<u64>> = (0..width).map(|_| Vec::with_capacity(n)).collect();
    let mut report = PartReport { part: name.clone(), ..Default::default() };
    let mut raw_digest = Sha256::new();

    let mut i = 0;
    while i < n {
        let slot = master[i].slot;
        let mut j = i;
        while j < n && master[j].slot == slot {
            j += 1;
        }
        let block = source
            .read_slot(slot)?
            .ok_or_else(|| format!("{name}: slot {slot} has master transactions but no raw block in {}", source.describe()))?;
        report.slots += 1;
        raw_digest.update(block.source_sha256);
        for m in &master[i..j] {
            let pos = block
                .txs
                .binary_search_by_key(&m.tx_index, |t| t.tx_index)
                .map_err(|_| format!("slot {slot} tx_index {} (master_tx_id {}) missing from raw source", m.tx_index, m.mtx))?;
            let tx = &block.txs[pos];
            let sig = first_signature_b58(&tx.raw).unwrap_or_default();
            if sig != m.signature {
                return Err(format!(
                    "signature mismatch at slot {slot} tx_index {}: raw {sig} vs labels {}",
                    m.tx_index, m.signature
                )
                .into());
            }
            if let Some(e) = tx.entry_index {
                if e != m.entry_index {
                    return Err(format!("entry_index mismatch at slot {slot} tx_index {}: raw {e} vs labels {}", m.tx_index, m.entry_index).into());
                }
            }
            report.v1_format += is_v1_format(&tx.raw) as u64;
            let status = encode_transaction_v2(&tx.raw, &mut scratch, &mut enc);
            let rel = if status == Status::Ok { universe.relevance(&enc.base, &tx.raw) } else { Default::default() };
            gate[0].push(rel.market_writable);
            gate[1].push(rel.market_readonly);
            gate[2].push(rel.venue_program);
            relevant.push(rel.relevant() as u8);
            report.relevant += rel.relevant() as u64;
            if status == Status::Ok {
                flatten::flatten_v2(&enc, cb_hash, &mut row);
                report.encode_ok += 1;
                for ix in &enc.base.instructions[..enc.base.header.instruction_count as usize] {
                    if ix.program_identity_available == 1 {
                        let off = ix.program_key_offset as usize;
                        report
                            .programs
                            .entry(format!("{:016x}", ix.program_identity_hash64))
                            .or_insert_with(|| bs58::encode(&tx.raw[off..off + 32]).into_string());
                    }
                }
            } else {
                row.fill(0);
                report.encode_failed += 1;
            }
            mtx.push(m.mtx);
            slots.push(slot);
            block_time.push(block.block_time);
            status_col.push(status.as_i32());
            for (col, v) in values.iter_mut().zip(row.iter()) {
                col.push(*v);
            }
        }
        i = j;
    }
    report.rows = n as u64;
    report.raw_digest = hex(&raw_digest.finalize());

    let specs = flatten::feature_specs();
    let mut cols: Vec<ArrayRef> = vec![
        Arc::new(UInt64Array::from(mtx)),
        Arc::new(UInt64Array::from(slots)),
        Arc::new(Int64Array::from(block_time)),
        Arc::new(Int32Array::from(status_col)),
    ];
    for g in gate {
        cols.push(Arc::new(UInt16Array::from(g)));
    }
    cols.push(Arc::new(UInt8Array::from(relevant)));
    for (spec, col) in specs.iter().zip(values) {
        cols.push(pq::narrow(spec.width, col.into_iter()));
    }
    pq::write_file(out_part, schema(), cols)?;
    Ok(report)
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
