//! Parquet helpers: projected streaming reads and deterministic, atomic writes.

use std::collections::HashMap;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow_array::cast::AsArray;
use arrow_array::types::{Int64Type, UInt32Type, UInt64Type, UInt8Type};
use arrow_array::{Array, ArrayRef, RecordBatch};
use arrow_schema::{DataType, Field, Schema};
use parquet::arrow::arrow_reader::{ParquetRecordBatchReader, ParquetRecordBatchReaderBuilder};
use parquet::arrow::{ArrowWriter, ProjectionMask};
use parquet::basic::{Compression, ZstdLevel};
use parquet::file::properties::WriterProperties;

use crate::Result;

/// Rows per record batch. Index `route_ids` cells hold thousands of route ids each, so large
/// batches overflow arrow's 32-bit string offsets ("index overflow decoding byte array").
pub const READ_BATCH: usize = 1024;
pub const ROW_GROUP: usize = 131_072;

/// Streams `columns` (by name) from `path`. Missing columns are an error.
pub fn open(path: &Path, columns: &[&str]) -> Result<(ParquetRecordBatchReader, Vec<usize>)> {
    let file = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file)?;
    let schema = builder.schema().clone();
    let mut leaves = Vec::new();
    for c in columns {
        let idx = schema
            .index_of(c)
            .map_err(|_| format!("{}: missing column {c}", path.display()))?;
        leaves.push(idx);
    }
    let mut sorted = leaves.clone();
    sorted.sort();
    let mask = ProjectionMask::roots(builder.parquet_schema(), sorted.clone());
    let reader = builder.with_projection(mask).with_batch_size(READ_BATCH).build()?;
    // Map requested order -> position inside the projected batch.
    let order = leaves.iter().map(|l| sorted.iter().position(|s| s == l).unwrap()).collect();
    Ok((reader, order))
}

pub fn num_rows(path: &Path) -> Result<u64> {
    let file = File::open(path)?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file)?;
    Ok(builder.metadata().file_metadata().num_rows() as u64)
}

/// Typed column views over one batch.
pub struct Cols<'a> {
    pub batch: &'a RecordBatch,
    pub order: &'a [usize],
}

impl<'a> Cols<'a> {
    fn col(&self, i: usize) -> &'a ArrayRef {
        self.batch.column(self.order[i])
    }
    pub fn u64(&self, i: usize) -> Result<&'a arrow_array::PrimitiveArray<UInt64Type>> {
        self.col(i).as_primitive_opt::<UInt64Type>().ok_or_else(|| format!("column {i} is not u64").into())
    }
    pub fn u32(&self, i: usize) -> Result<&'a arrow_array::PrimitiveArray<UInt32Type>> {
        self.col(i).as_primitive_opt::<UInt32Type>().ok_or_else(|| format!("column {i} is not u32").into())
    }
    pub fn u8(&self, i: usize) -> Result<&'a arrow_array::PrimitiveArray<UInt8Type>> {
        self.col(i).as_primitive_opt::<UInt8Type>().ok_or_else(|| format!("column {i} is not u8").into())
    }
    pub fn i64(&self, i: usize) -> Result<&'a arrow_array::PrimitiveArray<Int64Type>> {
        self.col(i).as_primitive_opt::<Int64Type>().ok_or_else(|| format!("column {i} is not i64").into())
    }
    pub fn str(&self, i: usize) -> Result<&'a arrow_array::StringArray> {
        self.col(i).as_string_opt::<i32>().ok_or_else(|| format!("column {i} is not utf8").into())
    }
    pub fn bool(&self, i: usize) -> Result<&'a arrow_array::BooleanArray> {
        self.col(i).as_boolean_opt().ok_or_else(|| format!("column {i} is not bool").into())
    }
}

pub fn opt_i64(a: &arrow_array::PrimitiveArray<Int64Type>, i: usize) -> Option<i64> {
    if a.is_null(i) {
        None
    } else {
        Some(a.value(i))
    }
}

/// A field with `role` (and optional extra) metadata.
pub fn field(name: &str, dtype: DataType, nullable: bool, role: &str, extra: &[(&str, &str)]) -> Field {
    let mut meta = HashMap::new();
    meta.insert("role".to_string(), role.to_string());
    for (k, v) in extra {
        meta.insert(k.to_string(), v.to_string());
    }
    Field::new(name, dtype, nullable).with_metadata(meta)
}

pub fn width_type(width: u8) -> DataType {
    match width {
        1 => DataType::UInt8,
        2 => DataType::UInt16,
        4 => DataType::UInt32,
        _ => DataType::UInt64,
    }
}

pub fn narrow(width: u8, values: impl Iterator<Item = u64>) -> ArrayRef {
    use arrow_array::{UInt16Array, UInt32Array, UInt64Array, UInt8Array};
    match width {
        1 => Arc::new(UInt8Array::from_iter_values(values.map(|v| v as u8))),
        2 => Arc::new(UInt16Array::from_iter_values(values.map(|v| v as u16))),
        4 => Arc::new(UInt32Array::from_iter_values(values.map(|v| v as u32))),
        _ => Arc::new(UInt64Array::from_iter_values(values)),
    }
}

fn props() -> WriterProperties {
    WriterProperties::builder()
        .set_compression(Compression::ZSTD(ZstdLevel::try_new(3).expect("zstd level")))
        .set_max_row_group_size(ROW_GROUP)
        .set_created_by("ml-prep".to_string())
        .build()
}

/// Writes one complete file: `path.tmp`, then rename. A reader never sees a
/// partial file at `path`, which is what makes per-part resume safe.
pub fn write_file(path: &Path, schema: Arc<Schema>, columns: Vec<ArrayRef>) -> Result<()> {
    let batch = if columns.is_empty() {
        RecordBatch::new_empty(schema.clone())
    } else {
        RecordBatch::try_new(schema.clone(), columns)?
    };
    let tmp = tmp_path(path);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    {
        let mut writer = ArrowWriter::try_new(File::create(&tmp)?, schema, Some(props()))?;
        writer.write(&batch)?;
        writer.close()?;
    }
    std::fs::rename(&tmp, path)?;
    Ok(())
}

pub fn tmp_path(path: &Path) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(".tmp");
    PathBuf::from(s)
}

pub fn write_json<T: serde::Serialize>(path: &Path, value: &T) -> Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = tmp_path(path);
    let mut text = serde_json::to_string_pretty(value)?;
    text.push('\n');
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}
