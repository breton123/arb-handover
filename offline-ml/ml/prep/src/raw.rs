//! Raw serialized transactions by slot.
//!
//! All sources share one addressing scheme: `tx_index` is the transaction's
//! position in the block across all entries.
//! - Old Faithful `{slot}.entries.json.zst` (the 10k-slot run).
//! - Compacted fleet dataset partitions (`part-*.transactions.parquet`, see
//!   [`crate::dataset_source`]) for the weekly labelled partitions.
//! - `raw-tx` JSONL (`slot, tx_index, signature, raw_hex`) for small local development.

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use sha2::{Digest, Sha256};

use crate::dataset_source::DatasetSource;
use crate::Result;

pub struct RawTx {
    pub tx_index: u64,
    /// Entry number. Unknown for JSONL input.
    pub entry_index: Option<u64>,
    pub raw: Vec<u8>,
}

pub struct SlotTxs {
    pub block_time: Option<i64>,
    pub txs: Vec<RawTx>,
    /// sha256 of the source bytes for this slot (compressed file, or the JSONL lines).
    pub source_sha256: [u8; 32],
}

pub enum RawSource {
    Entries { dir: PathBuf },
    Jsonl { path: PathBuf, slots: BTreeMap<u64, SlotTxs> },
    Dataset(Box<DatasetSource>),
}

#[derive(serde::Deserialize)]
struct ArchivedBlock {
    slot: u64,
    block_time: Option<i64>,
    entries: Vec<ArchivedEntry>,
}

#[derive(serde::Deserialize)]
struct ArchivedEntry {
    transactions: Vec<Vec<u8>>,
}

#[derive(serde::Deserialize)]
struct JsonlRow {
    slot: u64,
    tx_index: u64,
    raw_hex: String,
}

impl RawSource {
    pub fn entries(dir: &Path) -> Result<Self> {
        if !dir.is_dir() {
            return Err(format!("raw entries dir {} does not exist", dir.display()).into());
        }
        Ok(RawSource::Entries { dir: dir.to_path_buf() })
    }

    /// Loads the whole file. Development and tests only.
    pub fn jsonl(path: &Path) -> Result<Self> {
        let file = std::fs::File::open(path)?;
        let mut slots: BTreeMap<u64, (Vec<RawTx>, Sha256)> = BTreeMap::new();
        for (n, line) in BufReader::new(file).lines().enumerate() {
            let line = line?;
            if line.trim().is_empty() {
                continue;
            }
            let row: JsonlRow = serde_json::from_str(&line)
                .map_err(|e| format!("{}:{}: {e}", path.display(), n + 1))?;
            let raw = decode_hex(&row.raw_hex).map_err(|e| format!("{}:{}: {e}", path.display(), n + 1))?;
            let entry = slots.entry(row.slot).or_insert_with(|| (Vec::new(), Sha256::new()));
            entry.1.update(line.as_bytes());
            entry.0.push(RawTx { tx_index: row.tx_index, entry_index: None, raw });
        }
        let slots = slots
            .into_iter()
            .map(|(slot, (mut txs, hasher))| {
                txs.sort_by_key(|t| t.tx_index);
                (slot, SlotTxs { block_time: None, txs, source_sha256: hasher.finalize().into() })
            })
            .collect();
        Ok(RawSource::Jsonl { path: path.to_path_buf(), slots })
    }

    /// `labels_dir` supplies block times (the dataset has none).
    pub fn dataset(dir: &Path, labels_dir: &Path, cached_parts: usize) -> Result<Self> {
        Ok(RawSource::Dataset(Box::new(DatasetSource::open(dir, labels_dir, cached_parts)?)))
    }

    pub fn describe(&self) -> String {
        match self {
            RawSource::Dataset(d) => format!("dataset:{} ({} parts)", d.dir.display(), d.part_count()),
            RawSource::Entries { dir } => format!("entries:{}", dir.display()),
            RawSource::Jsonl { path, .. } => format!("jsonl:{}", path.display()),
        }
    }

    pub fn kind(&self) -> &'static str {
        match self {
            RawSource::Entries { .. } => "old-faithful-entries-json-zst",
            RawSource::Jsonl { .. } => "raw-tx-jsonl",
            RawSource::Dataset(_) => "fleet-dataset-transactions-parquet",
        }
    }

    /// `None` when the source has no block for this slot (skipped slot, or
    /// outside a JSONL sample).
    pub fn read_slot(&self, slot: u64) -> Result<Option<SlotTxsRef<'_>>> {
        match self {
            RawSource::Entries { dir } => {
                let path = dir.join(format!("{slot}.entries.json.zst"));
                if !path.exists() {
                    return Ok(None);
                }
                let compressed = std::fs::read(&path)?;
                let sha: [u8; 32] = Sha256::digest(&compressed).into();
                let json = zstd::decode_all(compressed.as_slice())
                    .map_err(|e| format!("{}: zstd: {e}", path.display()))?;
                let block: ArchivedBlock = serde_json::from_slice(&json)
                    .map_err(|e| format!("{}: json: {e}", path.display()))?;
                if block.slot != slot {
                    return Err(format!("{}: block slot {} != file slot {slot}", path.display(), block.slot).into());
                }
                let mut txs = Vec::new();
                let mut tx_index = 0u64;
                for (entry_index, entry) in block.entries.into_iter().enumerate() {
                    for raw in entry.transactions {
                        txs.push(RawTx { tx_index, entry_index: Some(entry_index as u64), raw });
                        tx_index += 1;
                    }
                }
                Ok(Some(SlotTxsRef::Owned(SlotTxs { block_time: block.block_time, txs, source_sha256: sha })))
            }
            RawSource::Jsonl { slots, .. } => Ok(slots.get(&slot).map(SlotTxsRef::Borrowed)),
            RawSource::Dataset(d) => Ok(d.read_slot(slot)?.map(SlotTxsRef::Shared)),
        }
    }
}

pub enum SlotTxsRef<'a> {
    Owned(SlotTxs),
    Borrowed(&'a SlotTxs),
    Shared(Arc<SlotTxs>),
}

impl std::ops::Deref for SlotTxsRef<'_> {
    type Target = SlotTxs;
    fn deref(&self) -> &SlotTxs {
        match self {
            SlotTxsRef::Owned(s) => s,
            SlotTxsRef::Borrowed(s) => s,
            SlotTxsRef::Shared(s) => s,
        }
    }
}

/// V1 transaction prefix (`MESSAGE_VERSION_PREFIX | 1`, solana-message 4.x).
pub const V1_PREFIX: u8 = 0x81;

/// Base58 of the first signature. Used only to verify the join; signatures are
/// not features.
///
/// Legacy and v0 transactions start with a compact-u16 signature count and the
/// signatures. V1 transactions are `0x81 | message | signatures`: the
/// signatures trail the message as a fixed array of `num_required_signatures`
/// (the first header byte, right after the prefix).
pub fn first_signature_b58(raw: &[u8]) -> Option<String> {
    if raw.first() == Some(&V1_PREFIX) {
        let n = *raw.get(1)? as usize;
        let start = raw.len().checked_sub(64 * n)?;
        if n == 0 || start < 2 {
            return None;
        }
        return Some(bs58::encode(&raw[start..start + 64]).into_string());
    }
    let (count, used) = compact_u16(raw)?;
    if count == 0 || raw.len() < used + 64 {
        return None;
    }
    Some(bs58::encode(&raw[used..used + 64]).into_string())
}

/// `true` for the v1 transaction format, which `framed-tx-v1` does not parse.
pub fn is_v1_format(raw: &[u8]) -> bool {
    raw.first() == Some(&V1_PREFIX)
}

fn compact_u16(b: &[u8]) -> Option<(u16, usize)> {
    let mut value: u32 = 0;
    for i in 0..3 {
        let byte = *b.get(i)?;
        value |= ((byte & 0x7f) as u32) << (7 * i);
        if byte & 0x80 == 0 {
            return u16::try_from(value).ok().map(|v| (v, i + 1));
        }
    }
    None
}

pub fn decode_hex(s: &str) -> std::result::Result<Vec<u8>, String> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return Err("odd hex length".into());
    }
    let val = |c: u8| match c {
        b'0'..=b'9' => Ok(c - b'0'),
        b'a'..=b'f' => Ok(c - b'a' + 10),
        b'A'..=b'F' => Ok(c - b'A' + 10),
        _ => Err(format!("bad hex byte {c}")),
    };
    b.chunks(2).map(|p| Ok((val(p[0])? << 4) | val(p[1])?)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_signature_in_both_layouts() {
        let sig: Vec<u8> = (0u8..64).collect();
        let mut legacy = vec![1u8];
        legacy.extend(&sig);
        legacy.extend([1, 0, 0]);
        let mut v1 = vec![V1_PREFIX, 1, 0, 0, 7, 7, 7];
        v1.extend(&sig);
        let want = bs58::encode(&sig).into_string();
        assert_eq!(first_signature_b58(&legacy).as_deref(), Some(want.as_str()));
        assert_eq!(first_signature_b58(&v1).as_deref(), Some(want.as_str()));
        assert!(is_v1_format(&v1) && !is_v1_format(&legacy));
        assert_eq!(first_signature_b58(&[V1_PREFIX, 2, 0]), None);
    }
}
