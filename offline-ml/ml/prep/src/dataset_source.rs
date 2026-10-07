//! Raw transactions from a compacted fleet dataset partition.
//!
//! Layout (the `dataset/` directory of a published source partition):
//! `compaction.json` lists parts with `first_slot..=last_slot`, and
//! `part-NNNNNN.transactions.parquet` holds one row per transaction:
//! `slot, entry_index, tx_index, signature, record_cbor`. `record_cbor` is the
//! dataset-reader `Transaction` record; its `raw` field is the serialized
//! transaction as a CBOR byte string. Only `raw` is read; every other field of
//! the record (keys, instructions, CU) is ignored because features come only
//! from `encode_transaction_v2(raw)`.
//!
//! The dataset has no block times. They come from the labels' `labels.parquet`
//! `timestamp` (minimum per slot), carried forward to slots without a label,
//! and are used only for chain-hour denominators.

use std::collections::{BTreeMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use arrow_array::cast::AsArray;
use arrow_array::Array;
use sha2::{Digest, Sha256};

use crate::pq::{self, Cols};
use crate::raw::{RawTx, SlotTxs};
use crate::Result;

type Part = Arc<BTreeMap<u64, Arc<SlotTxs>>>;

pub struct DatasetSource {
    pub dir: PathBuf,
    /// (first_slot, last_slot, transactions parquet), sorted by first_slot.
    parts: Vec<(u64, u64, PathBuf)>,
    cache: Mutex<VecDeque<(usize, Part)>>,
    capacity: usize,
    slot_times: BTreeMap<u64, i64>,
}

impl DatasetSource {
    /// `dir` is a dataset directory (holding `compaction.json`) or its parent.
    pub fn open(dir: &Path, labels_dir: &Path, capacity: usize) -> Result<Self> {
        let dir = if dir.join("compaction.json").exists() { dir.to_path_buf() } else { dir.join("dataset") };
        let compaction: serde_json::Value = serde_json::from_str(
            &std::fs::read_to_string(dir.join("compaction.json"))
                .map_err(|e| format!("{}: {e}", dir.join("compaction.json").display()))?,
        )?;
        let mut parts = Vec::new();
        for p in compaction["partitions"].as_array().ok_or("compaction.json: partitions")? {
            let part = p["part"].as_u64().ok_or("compaction.json: part")?;
            let first = p["first_slot"].as_u64().ok_or("compaction.json: first_slot")?;
            let last = p["last_slot"].as_u64().ok_or("compaction.json: last_slot")?;
            parts.push((first, last, dir.join(format!("part-{part:06}.transactions.parquet"))));
        }
        parts.sort();
        for w in parts.windows(2) {
            if w[0].1 >= w[1].0 {
                return Err("compaction.json parts overlap".into());
            }
        }
        if let Some((_, _, path)) = parts.iter().find(|(_, _, p)| !p.exists()) {
            return Err(format!("missing dataset part {}", path.display()).into());
        }
        Ok(Self { dir, parts, cache: Mutex::new(VecDeque::new()), capacity, slot_times: slot_times(labels_dir)? })
    }

    pub fn part_count(&self) -> usize {
        self.parts.len()
    }

    pub fn read_slot(&self, slot: u64) -> Result<Option<Arc<SlotTxs>>> {
        let i = match self.parts.binary_search_by(|(first, last, _)| {
            if slot < *first {
                std::cmp::Ordering::Greater
            } else if slot > *last {
                std::cmp::Ordering::Less
            } else {
                std::cmp::Ordering::Equal
            }
        }) {
            Ok(i) => i,
            Err(_) => return Ok(None),
        };
        let part = self.part(i)?;
        Ok(part.get(&slot).cloned())
    }

    fn part(&self, i: usize) -> Result<Part> {
        if let Some((_, p)) = self.cache.lock().unwrap().iter().find(|(j, _)| *j == i) {
            return Ok(p.clone());
        }
        let p = Arc::new(self.load(i)?);
        let mut cache = self.cache.lock().unwrap();
        if !cache.iter().any(|(j, _)| *j == i) {
            cache.push_back((i, p.clone()));
            while cache.len() > self.capacity {
                cache.pop_front();
            }
        }
        Ok(p)
    }

    fn load(&self, i: usize) -> Result<BTreeMap<u64, Arc<SlotTxs>>> {
        let path = &self.parts[i].2;
        let (reader, order) = pq::open(path, &["slot", "entry_index", "tx_index", "record_cbor"])?;
        let mut slots: BTreeMap<u64, (Vec<RawTx>, Sha256)> = BTreeMap::new();
        for batch in reader {
            let batch = batch?;
            let c = Cols { batch: &batch, order: &order };
            let (slots_col, entries, txi) = (c.u64(0)?, c.u64(1)?, c.u64(2)?);
            let records = batch.column(order[3]).as_binary_opt::<i32>().ok_or("record_cbor is not binary")?;
            for r in 0..batch.num_rows() {
                let record = records.value(r);
                let raw = cbor_map_bytes(record, "raw")
                    .ok_or_else(|| format!("{}: row {r}: record has no raw byte string", path.display()))?;
                let (txs, hasher) = slots.entry(slots_col.value(r)).or_insert_with(|| (Vec::new(), Sha256::new()));
                hasher.update(record);
                txs.push(RawTx {
                    tx_index: txi.value(r),
                    entry_index: (!entries.is_null(r)).then(|| entries.value(r)),
                    raw: raw.to_vec(),
                });
            }
        }
        Ok(slots
            .into_iter()
            .map(|(slot, (mut txs, h))| {
                txs.sort_by_key(|t| t.tx_index);
                let block_time = self.slot_times.range(..=slot).next_back().map(|(_, t)| *t);
                (slot, Arc::new(SlotTxs { block_time, txs, source_sha256: h.finalize().into() }))
            })
            .collect())
    }
}

/// Minimum `labels.parquet` timestamp per slot.
fn slot_times(labels_dir: &Path) -> Result<BTreeMap<u64, i64>> {
    let path = labels_dir.join("labels.parquet");
    let mut out: BTreeMap<u64, i64> = BTreeMap::new();
    if !path.exists() {
        return Ok(out);
    }
    let (reader, order) = pq::open(&path, &["slot", "timestamp"])?;
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        let (slot, ts) = (c.u64(0)?, c.i64(1)?);
        for r in 0..batch.num_rows() {
            if let Some(t) = pq::opt_i64(ts, r) {
                out.entry(slot.value(r)).and_modify(|v| *v = (*v).min(t)).or_insert(t);
            }
        }
    }
    Ok(out)
}

// ---------------------------------------------------------------- CBOR

/// Header: (major type, argument, indefinite length). Advances `i` past the
/// header and its argument bytes (which include float payloads for major 7).
fn head(b: &[u8], i: &mut usize) -> Option<(u8, u64, bool)> {
    let ib = *b.get(*i)?;
    *i += 1;
    let (major, ai) = (ib >> 5, ib & 0x1f);
    let mut take = |n: usize| -> Option<u64> {
        let s = b.get(*i..*i + n)?;
        *i += n;
        Some(s.iter().fold(0u64, |a, x| (a << 8) | *x as u64))
    };
    match ai {
        0..=23 => Some((major, ai as u64, false)),
        24 => Some((major, take(1)?, false)),
        25 => Some((major, take(2)?, false)),
        26 => Some((major, take(4)?, false)),
        27 => Some((major, take(8)?, false)),
        31 if matches!(major, 2..=5) => Some((major, 0, true)),
        _ => None,
    }
}

fn skip(b: &[u8], i: &mut usize, depth: u32) -> Option<()> {
    if depth > 64 {
        return None;
    }
    let (major, arg, indefinite) = head(b, i)?;
    match major {
        0 | 1 | 7 => Some(()),
        2 | 3 if !indefinite => {
            let end = i.checked_add(arg as usize)?;
            (end <= b.len()).then(|| *i = end)
        }
        2..=5 if indefinite => {
            while *b.get(*i)? != 0xff {
                skip(b, i, depth + 1)?;
            }
            *i += 1;
            Some(())
        }
        4 => (0..arg).try_for_each(|_| skip(b, i, depth + 1)),
        5 => (0..arg * 2).try_for_each(|_| skip(b, i, depth + 1)),
        6 => skip(b, i, depth + 1),
        _ => None,
    }
}

/// The definite byte string stored under text key `key` in a top-level CBOR map.
pub fn cbor_map_bytes<'a>(b: &'a [u8], key: &str) -> Option<&'a [u8]> {
    let mut i = 0;
    let (major, n, indefinite) = head(b, &mut i)?;
    if major != 5 {
        return None;
    }
    let mut left = n;
    loop {
        if indefinite {
            if *b.get(i)? == 0xff {
                return None;
            }
        } else if left == 0 {
            return None;
        } else {
            left -= 1;
        }
        let start = i;
        let (km, klen, kindef) = head(b, &mut i)?;
        let matched = if km == 3 && !kindef {
            let k = b.get(i..i + klen as usize)?;
            i += klen as usize;
            k == key.as_bytes()
        } else {
            i = start;
            skip(b, &mut i, 0)?;
            false
        };
        if matched {
            let (vm, vlen, vindef) = head(b, &mut i)?;
            return if vm == 2 && !vindef { b.get(i..i + vlen as usize) } else { None };
        }
        skip(b, &mut i, 0)?;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// {"slot": 500, "nested": [1, -2, {"f": 1.5}, h'00ff', "txt", true, null], "big": 2^40, "raw": h'010203'}
    fn sample() -> Vec<u8> {
        let mut v = vec![0xa4];
        v.extend([0x64, b's', b'l', b'o', b't', 0x19, 0x01, 0xf4]);
        v.extend([0x66, b'n', b'e', b's', b't', b'e', b'd', 0x87, 0x01, 0x21]);
        v.extend([0xa1, 0x61, b'f', 0xfb, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0]);
        v.extend([0x42, 0x00, 0xff, 0x63, b't', b'x', b't', 0xf5, 0xf6]);
        v.extend([0x63, b'b', b'i', b'g', 0x1b, 0, 0, 1, 0, 0, 0, 0, 0]);
        v.extend([0x63, b'r', b'a', b'w', 0x43, 1, 2, 3]);
        v
    }

    #[test]
    fn finds_raw_after_skipping_every_value_kind() {
        assert_eq!(cbor_map_bytes(&sample(), "raw"), Some(&[1u8, 2, 3][..]));
        assert_eq!(cbor_map_bytes(&sample(), "missing"), None);
        assert_eq!(cbor_map_bytes(&sample(), "slot"), None); // not a byte string
    }

    #[test]
    fn truncated_input_is_rejected_not_misread() {
        let s = sample();
        for cut in 0..s.len() - 1 {
            assert_eq!(cbor_map_bytes(&s[..cut], "raw"), None, "cut at {cut}");
        }
    }

    #[test]
    fn indefinite_map_and_strings() {
        // {_ "a": (_ h'01' h'02'), "raw": h'09' }
        let v = [0xbf, 0x61, b'a', 0x5f, 0x41, 1, 0x41, 2, 0xff, 0x63, b'r', b'a', b'w', 0x41, 9, 0xff];
        assert_eq!(cbor_map_bytes(&v, "raw"), Some(&[9u8][..]));
    }
}
