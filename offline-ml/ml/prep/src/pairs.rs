//! Pairs pass: expand `(transaction, eligible pack)` rows up to catalog depth.
//!
//! Runs per part after the features pass, because which UNKNOWN transactions
//! get pairs depends on the relevance gate. Rows:
//!
//! - POSITIVE: every eligible pack, all layers (oracle and live availability, ranker training)
//! - CERTIFIED_NEGATIVE: live (TOP_LEVEL-key) packs (send precision of the decision policies)
//! - UNKNOWN: live packs, only when the transaction is relevant and in the
//!   deterministic `pair_sample` (live send volume of the joint policy)
//!
//! A pack is eligible at depth k when its catalog rank within its key is < k.

use std::collections::BTreeSet;
use std::path::Path;
use std::sync::Arc;

use arrow_array::cast::AsArray;
use arrow_array::types::UInt32Type;
use arrow_array::{ArrayRef, BooleanArray, Int8Array, StringArray, UInt32Array, UInt64Array};
use arrow_schema::{DataType, Schema};
use serde::{Deserialize, Serialize};

use crate::labels::Key;
use crate::packs::PackIndex;
use crate::pq::{self, Cols};
use crate::Result;

pub const PAIRS_SCHEMA: &str = "ml-pack-pairs-2";

#[derive(Serialize, Deserialize, Clone, Default)]
pub struct PairReport {
    pub part: String,
    pub rows: u64,
    pub positive_target: u64,
    pub transactions: u64,
}

pub fn schema() -> Arc<Schema> {
    use pq::field;
    Arc::new(Schema::new_with_metadata(
        vec![
            field("master_tx_id", DataType::UInt64, false, "id", &[]),
            field("slot", DataType::UInt64, false, "id", &[]),
            field("pack_id", DataType::Utf8, false, "id", &[]),
            field("pack_rank", DataType::UInt32, false, "meta", &[]),
            field("arb_label", DataType::Utf8, false, "label", &[]),
            field("pack_target", DataType::Int8, true, "label", &[]),
            field("eligible_live", DataType::Boolean, false, "meta", &[]),
        ],
        [("schema".to_string(), PAIRS_SCHEMA.to_string())].into(),
    ))
}

pub fn build_part(
    labels_part: &Path,
    features_part: &Path,
    out_part: &Path,
    keys: &[Key],
    packs: &PackIndex,
    max_depth: u32,
) -> Result<PairReport> {
    let mut report = PairReport { part: labels_part.file_name().unwrap().to_string_lossy().to_string(), ..Default::default() };
    // Relevance, aligned row-for-row with tx_labels.
    let mut relevant: Vec<(u64, u8)> = Vec::new();
    let (reader, order) = pq::open(features_part, &["master_tx_id", "relevant"])?;
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        for i in 0..batch.num_rows() {
            relevant.push((c.u64(0)?.value(i), c.u8(1)?.value(i)));
        }
    }

    let (mut mtx, mut slots, mut ids, mut ranks, mut labels, mut targets, mut live) =
        (vec![], vec![], vec![], vec![], vec![], vec![], vec![]);
    let cols = ["master_tx_id", "slot", "arb_label", "trigger_keys", "profitable_routes", "pair_sample"];
    let (reader, order) = pq::open(labels_part, &cols)?;
    let mut row = 0usize;
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        let key_lists = batch.column(order[3]).as_list::<i32>();
        for i in 0..batch.num_rows() {
            let m = c.u64(0)?.value(i);
            let (fm, rel) = relevant.get(row).copied().ok_or("tx_features shorter than tx_labels")?;
            if fm != m {
                return Err(format!("{}: tx_labels/tx_features rows misaligned at {m}", report.part).into());
            }
            row += 1;
            let label = c.str(2)?.value(i);
            let (all_layers, target) = match label {
                "POSITIVE" => (true, true),
                "CERTIFIED_NEGATIVE" => (false, true),
                _ if rel == 1 && c.u8(5)?.value(i) == 1 => (false, false),
                _ => continue,
            };
            let profitable: BTreeSet<&str> = c.str(4)?.value(i).split(',').filter(|x| !x.is_empty()).collect();
            let key_ids = key_lists.value(i);
            let key_ids = key_ids.as_primitive::<UInt32Type>();
            let mut eligible: BTreeSet<usize> = BTreeSet::new();
            for k in key_ids.values().iter() {
                let (p, f, l, b) = &keys[*k as usize];
                for &pk in packs.eligible(p, f, l, b) {
                    if packs.packs[pk].rank < max_depth && (all_layers || packs.is_live(pk)) {
                        eligible.insert(pk);
                    }
                }
            }
            if eligible.is_empty() {
                continue;
            }
            report.transactions += 1;
            for pk in eligible {
                let pack = &packs.packs[pk];
                let t = target.then(|| pack.route_ids.iter().any(|r| profitable.contains(r.as_str())) as i8);
                report.positive_target += (t == Some(1)) as u64;
                mtx.push(m);
                slots.push(c.u64(1)?.value(i));
                ids.push(pack.pack_id.clone());
                ranks.push(pack.rank);
                labels.push(label.to_string());
                targets.push(t);
                live.push(packs.is_live(pk));
            }
        }
    }
    if row != relevant.len() {
        return Err(format!("{}: tx_features longer than tx_labels", report.part).into());
    }
    report.rows = mtx.len() as u64;
    let cols: Vec<ArrayRef> = vec![
        Arc::new(UInt64Array::from(mtx)),
        Arc::new(UInt64Array::from(slots)),
        Arc::new(StringArray::from(ids)),
        Arc::new(UInt32Array::from(ranks)),
        Arc::new(StringArray::from(labels)),
        Arc::new(Int8Array::from(targets)),
        Arc::new(BooleanArray::from(live)),
    ];
    pq::write_file(out_part, schema(), cols)?;
    Ok(report)
}
