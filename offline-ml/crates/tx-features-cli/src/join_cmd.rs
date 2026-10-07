//! Generic left join of a label JSONL onto a feature parquet.
//!
//! Label fields are not interpreted. This binary does not link economic types
//! into the encoder.

use std::collections::{BTreeSet, HashMap};
use std::fs::File;
use std::path::Path;
use std::sync::Arc;

use arrow_array::{Array, ArrayRef, BooleanArray, Float64Array, Int64Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use parquet::arrow::ArrowWriter;
use parquet::basic::Compression;
use parquet::file::properties::WriterProperties;
use serde_json::Value;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Int,
    Float,
    Bool,
    Text,
}

pub fn join_labels(
    features: &Path,
    labels_path: &Path,
    output: &Path,
    on: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let labels_text = std::fs::read_to_string(labels_path)?;
    let mut labels: HashMap<String, serde_json::Map<String, Value>> = HashMap::new();
    let mut keys: BTreeSet<String> = BTreeSet::new();
    for (line_no, line) in labels_text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let value: Value = serde_json::from_str(line)
            .map_err(|err| format!("{}:{}: {err}", labels_path.display(), line_no + 1))?;
        let obj = value
            .as_object()
            .ok_or_else(|| format!("{}:{}: label row is not an object", labels_path.display(), line_no + 1))?;
        let key = obj
            .get(on)
            .and_then(|v| match v {
                Value::String(s) => Some(s.clone()),
                Value::Number(n) => Some(n.to_string()),
                _ => None,
            })
            .ok_or_else(|| format!("{}:{}: missing join key {on}", labels_path.display(), line_no + 1))?;
        for (name, _) in obj {
            if name != on {
                keys.insert(name.clone());
            }
        }
        labels.insert(key, obj.clone());
    }

    let mut kinds: HashMap<String, Kind> = HashMap::new();
    for key in &keys {
        let mut kind: Option<Kind> = None;
        for row in labels.values() {
            if let Some(value) = row.get(key) {
                if let Some(next) = classify(value) {
                    kind = Some(match kind {
                        None => next,
                        Some(prev) => merge(prev, next),
                    });
                }
            }
        }
        kinds.insert(key.clone(), kind.unwrap_or(Kind::Text));
    }

    let file = File::open(features)?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file)?;
    let in_schema = builder.schema().clone();
    let sig_idx = in_schema
        .index_of(on)
        .map_err(|_| format!("feature file has no column {on}"))?;
    let reader = builder.build()?;

    let mut fields: Vec<Field> = in_schema.fields().iter().map(|f| f.as_ref().clone()).collect();
    for key in &keys {
        let dtype = match kinds[key] {
            Kind::Int => DataType::Int64,
            Kind::Float => DataType::Float64,
            Kind::Bool => DataType::Boolean,
            Kind::Text => DataType::Utf8,
        };
        let mut meta = std::collections::HashMap::new();
        meta.insert("group".into(), "label".into());
        fields.push(
            Field::new(format!("label_{key}"), dtype, true).with_metadata(meta),
        );
    }
    let out_schema = Arc::new(Schema::new(fields));
    let props = WriterProperties::builder()
        .set_compression(Compression::SNAPPY)
        .build();
    let mut writer = ArrowWriter::try_new(File::create(output)?, out_schema.clone(), Some(props))?;
    let mut written = 0usize;
    for batch in reader {
        let batch = batch?;
        let mut columns: Vec<ArrayRef> = batch.columns().to_vec();
        let sigs = batch
            .column(sig_idx)
            .as_any()
            .downcast_ref::<StringArray>()
            .ok_or("join column is not utf8")?;
        for key in &keys {
            columns.push(label_column(sigs, &labels, key, kinds[key]));
        }
        let out = RecordBatch::try_new(out_schema.clone(), columns)?;
        written += out.num_rows();
        writer.write(&out)?;
    }
    writer.close()?;
    eprintln!(
        "join-labels rows={written} label_columns={} output={}",
        keys.len(),
        output.display()
    );
    Ok(())
}

fn label_column(
    sigs: &StringArray,
    labels: &HashMap<String, serde_json::Map<String, Value>>,
    key: &str,
    kind: Kind,
) -> ArrayRef {
    match kind {
        Kind::Int => {
            let values: Vec<Option<i64>> = (0..sigs.len())
                .map(|i| lookup(sigs, labels, i, key).and_then(|v| v.as_i64()))
                .collect();
            Arc::new(Int64Array::from(values))
        }
        Kind::Float => {
            let values: Vec<Option<f64>> = (0..sigs.len())
                .map(|i| lookup(sigs, labels, i, key).and_then(|v| v.as_f64()))
                .collect();
            Arc::new(Float64Array::from(values))
        }
        Kind::Bool => {
            let values: Vec<Option<bool>> = (0..sigs.len())
                .map(|i| lookup(sigs, labels, i, key).and_then(|v| v.as_bool()))
                .collect();
            Arc::new(BooleanArray::from(values))
        }
        Kind::Text => {
            let values: Vec<Option<String>> = (0..sigs.len())
                .map(|i| {
                    lookup(sigs, labels, i, key).map(|v| match v {
                        Value::String(s) => s.clone(),
                        other => other.to_string(),
                    })
                })
                .collect();
            Arc::new(StringArray::from(values))
        }
    }
}

fn lookup<'a>(
    sigs: &StringArray,
    labels: &'a HashMap<String, serde_json::Map<String, Value>>,
    i: usize,
    key: &str,
) -> Option<&'a Value> {
    if sigs.is_null(i) {
        return None;
    }
    labels.get(sigs.value(i))?.get(key)
}

fn classify(value: &Value) -> Option<Kind> {
    match value {
        Value::Null => None,
        Value::Bool(_) => Some(Kind::Bool),
        Value::Number(n) => {
            if n.as_i64().is_some() {
                Some(Kind::Int)
            } else {
                Some(Kind::Float)
            }
        }
        _ => Some(Kind::Text),
    }
}

fn merge(a: Kind, b: Kind) -> Kind {
    match (a, b) {
        (Kind::Text, _) | (_, Kind::Text) => Kind::Text,
        (Kind::Float, _) | (_, Kind::Float) => Kind::Float,
        (Kind::Int, Kind::Int) => Kind::Int,
        (Kind::Bool, Kind::Bool) => Kind::Bool,
        _ => Kind::Text,
    }
}
