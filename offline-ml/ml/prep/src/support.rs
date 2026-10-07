//! Multi-partition support for a candidate library built over many labelled
//! partitions (`candidate-packs build-library --economics I0 I1 ...`).
//!
//! The library identifies transactions by global id `partition_index << 40 |
//! master_tx_id` (partitions in chronological `--economics` order). One
//! `ml-prep build` still runs per partition; these helpers provide what a single
//! partition cannot see by itself:
//!
//! - [`masters`]: `master_tx_id -> slot` for one partition, so global slot
//!   windows can be mapped to global ids.
//! - [`library_support`]: route metadata for every library pack route (pack
//!   routes come from all train partitions' positives) and the set of RouteIds
//!   profitable anywhere in the library's training window.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow_array::{ArrayRef, StringArray, UInt64Array, UInt8Array};
use arrow_schema::{DataType, Schema};

use crate::pq::{self, Cols};
use crate::Result;

pub const PARTITION_SHIFT: u32 = 40;

pub fn global_id(partition_index: u64, master: u64) -> u64 {
    (partition_index << PARTITION_SHIFT) | master
}

/// Distinct `(master_tx_id, slot)` of one labels directory, sorted by master.
pub fn masters(trigger_occurrences: &Path, out: &Path) -> Result<u64> {
    let (reader, order) = pq::open(trigger_occurrences, &["master_tx_id", "slot"])?;
    let (mut ids, mut slots) = (Vec::new(), Vec::new());
    let mut last: Option<u64> = None;
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        let (m, s) = (c.u64(0)?, c.u64(1)?);
        for i in 0..batch.num_rows() {
            let id = m.value(i);
            match last {
                Some(l) if l == id => continue,
                Some(l) if l > id => return Err("trigger_occurrences is not sorted by master_tx_id".into()),
                _ => {}
            }
            last = Some(id);
            ids.push(id);
            slots.push(s.value(i));
        }
    }
    let n = ids.len() as u64;
    let schema = Arc::new(Schema::new(vec![
        pq::field("master_tx_id", DataType::UInt64, false, "id", &[]),
        pq::field("slot", DataType::UInt64, false, "id", &[]),
    ]));
    pq::write_file(out, schema, vec![Arc::new(UInt64Array::from(ids)), Arc::new(UInt64Array::from(slots))])?;
    Ok(n)
}

#[derive(serde::Serialize)]
pub struct SupportReport {
    pub pack_routes: usize,
    pub train_routes: usize,
    pub indexes: usize,
}

/// Writes `library_routes.parquet` (route_id, hops, program_ids,
/// economic_families for every pack route) and `train_routes.parquet` (RouteIds
/// of positives whose global id is inside `train`) into `out`.
pub fn library_support(library: &Path, indexes: &[PathBuf], train: (u64, u64), out: &Path) -> Result<SupportReport> {
    let mut wanted: BTreeSet<String> = BTreeSet::new();
    let (reader, order) = pq::open(&library.join("library_packs.parquet"), &["route_ids"])?;
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        for i in 0..batch.num_rows() {
            wanted.extend(c.str(0)?.value(i).split(',').filter(|x| !x.is_empty()).map(str::to_string));
        }
    }
    let mut routes: BTreeMap<String, (u8, String, String)> = BTreeMap::new();
    let mut train_routes: BTreeSet<String> = BTreeSet::new();
    for (p, dir) in indexes.iter().enumerate() {
        let (reader, order) = pq::open(&dir.join("routes.parquet"), &["route_id", "hops", "program_ids", "economic_families"])?;
        for batch in reader {
            let batch = batch?;
            let c = Cols { batch: &batch, order: &order };
            for i in 0..batch.num_rows() {
                let id = c.str(0)?.value(i);
                if wanted.contains(id) && !routes.contains_key(id) {
                    let row = (c.u8(1)?.value(i), c.str(2)?.value(i).to_string(), c.str(3)?.value(i).to_string());
                    routes.insert(id.to_string(), row);
                }
            }
        }
        let (reader, order) = pq::open(&dir.join("positives.parquet"), &["master_tx_id", "route_ids"])?;
        for batch in reader {
            let batch = batch?;
            let c = Cols { batch: &batch, order: &order };
            for i in 0..batch.num_rows() {
                let g = global_id(p as u64, c.u64(0)?.value(i));
                if (train.0..=train.1).contains(&g) {
                    train_routes.extend(c.str(1)?.value(i).split(',').filter(|x| !x.is_empty()).map(str::to_string));
                }
            }
        }
    }
    if let Some(missing) = wanted.iter().find(|r| !routes.contains_key(*r)) {
        return Err(format!("pack route {missing} is in no index routes.parquet").into());
    }
    let schema = Arc::new(Schema::new(vec![
        pq::field("route_id", DataType::Utf8, false, "id", &[]),
        pq::field("hops", DataType::UInt8, false, "meta", &[]),
        pq::field("program_ids", DataType::Utf8, false, "meta", &[]),
        pq::field("economic_families", DataType::Utf8, false, "meta", &[]),
    ]));
    let cols: Vec<ArrayRef> = vec![
        Arc::new(StringArray::from_iter_values(routes.keys())),
        Arc::new(UInt8Array::from_iter_values(routes.values().map(|r| r.0))),
        Arc::new(StringArray::from_iter_values(routes.values().map(|r| r.1.as_str()))),
        Arc::new(StringArray::from_iter_values(routes.values().map(|r| r.2.as_str()))),
    ];
    pq::write_file(&out.join("library_routes.parquet"), schema, cols)?;
    let schema = Arc::new(Schema::new(vec![pq::field("route_id", DataType::Utf8, false, "id", &[])]));
    pq::write_file(
        &out.join("train_routes.parquet"),
        schema,
        vec![Arc::new(StringArray::from_iter_values(train_routes.iter()))],
    )?;
    Ok(SupportReport { pack_routes: routes.len(), train_routes: train_routes.len(), indexes: indexes.len() })
}

pub fn read_route_set(path: &Path) -> Result<std::collections::HashSet<String>> {
    let (reader, order) = pq::open(path, &["route_id"])?;
    let mut out = std::collections::HashSet::new();
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        for i in 0..batch.num_rows() {
            out.insert(c.str(0)?.value(i).to_string());
        }
    }
    Ok(out)
}
