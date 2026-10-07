//! JSONL adapter. Feature values are read from `EncodedTxV1`.
//! This file does not parse Solana transactions.

use std::fs::File;
use std::path::Path;
use std::sync::Arc;

use arrow_array::builder::{FixedSizeBinaryBuilder, ListBuilder, UInt64Builder};
use arrow_array::{
    ArrayRef, Int32Array, RecordBatch, StringArray, UInt16Array, UInt32Array, UInt64Array, UInt8Array,
};
use arrow_schema::{DataType, Field, Schema};
use parquet::arrow::ArrowWriter;
use parquet::basic::Compression;
use parquet::file::properties::WriterProperties;
use tx_features::{
    encode_transaction_v1, AccountRoleV1, EncodedTxV1, InstructionFeaturesV1, Scratch, Status,
    TxHeaderV1, FEATURE_SCHEMA,
};

const BATCH: usize = 4096;

macro_rules! spec_mod {
    ($modname:ident, $ty:ty, $($name:ident : $group:literal : $width:literal),* $(,)?) => {
        mod $modname {
            use super::*;
            pub struct Spec {
                pub name: &'static str,
                pub group: &'static str,
                pub width: u8,
                pub get: fn(&$ty) -> u64,
            }
            $(
                fn $name(v: &$ty) -> u64 {
                    v.$name as u64
                }
            )*
            pub static SPECS: &[Spec] = &[
                $(
                    Spec {
                        name: stringify!($name),
                        group: $group,
                        width: $width,
                        get: $name,
                    },
                )*
            ];
        }
    };
}

spec_mod!(
    header_cols,
    TxHeaderV1,
    requested_cu_price : "structural" : 8,
    program_sequence_fingerprint : "identity" : 8,
    instruction_structure_fingerprint : "structural" : 8,
    topology_fingerprint : "structural" : 8,
    schema_id : "meta" : 4,
    raw_len : "structural" : 4,
    total_instruction_account_refs : "structural" : 4,
    reused_account_ref_count : "structural" : 4,
    total_instruction_data_bytes : "structural" : 4,
    max_instruction_data_len : "structural" : 4,
    min_instruction_data_len : "structural" : 4,
    compute_budget_flags : "structural" : 4,
    requested_cu_limit : "structural" : 4,
    requested_heap_bytes : "structural" : 4,
    requested_loaded_accounts_data_size : "structural" : 4,
    deprecated_requested_units : "structural" : 4,
    deprecated_additional_fee : "structural" : 4,
    signature_count : "structural" : 2,
    required_signature_count : "structural" : 2,
    readonly_signed_count : "structural" : 2,
    readonly_unsigned_count : "structural" : 2,
    static_account_count : "structural" : 2,
    total_account_reference_count : "structural" : 2,
    static_writable_count : "structural" : 2,
    static_readonly_count : "structural" : 2,
    static_signer_count : "structural" : 2,
    instruction_count : "structural" : 2,
    unique_program_reference_count : "structural" : 2,
    unique_instruction_account_refs : "structural" : 2,
    alt_lookup_count : "structural" : 2,
    alt_loaded_writable_count : "structural" : 2,
    alt_loaded_readonly_count : "structural" : 2,
    compute_budget_instruction_count : "structural" : 2,
    compute_budget_unknown_count : "structural" : 2,
    compute_budget_malformed_count : "structural" : 2,
    compute_budget_first_index : "structural" : 2,
    compute_budget_last_index : "structural" : 2,
    max_account_usage_count : "structural" : 2,
    max_writable_account_usage_count : "structural" : 2,
    instructions_with_shared_accounts : "structural" : 2,
    instructions_with_shared_writable_accounts : "structural" : 2,
    accounts_used_by_multiple_instructions : "structural" : 2,
    writable_accounts_used_by_multiple_instructions : "structural" : 2,
    adjacent_pairs_sharing_account : "structural" : 2,
    adjacent_pairs_sharing_writable : "structural" : 2,
    repeated_program_invocation_count : "structural" : 2,
    version : "structural" : 1,
    uses_alt : "structural" : 1,
    program_repeated : "structural" : 1,
);

spec_mod!(
    ix_cols,
    InstructionFeaturesV1,
    program_identity_hash64 : "identity" : 8,
    program_alt_identity_hash64 : "identity" : 8,
    structural_family_hash64 : "structural" : 8,
    writable_position_pattern : "structural" : 8,
    signer_position_pattern : "structural" : 8,
    prefix8 : "raw" : 8,
    data_offset : "raw" : 4,
    data_len : "structural" : 4,
    accounts_offset : "raw" : 4,
    accounts_len : "raw" : 4,
    program_key_offset : "raw" : 4,
    prefix4 : "raw" : 4,
    position : "structural" : 2,
    account_count : "structural" : 2,
    signer_account_count : "structural" : 2,
    writable_account_count : "structural" : 2,
    readonly_account_count : "structural" : 2,
    static_account_ref_count : "structural" : 2,
    alt_account_ref_count : "structural" : 2,
    reused_account_count : "structural" : 2,
    reused_writable_account_count : "structural" : 2,
    alt_table_ordinal : "identity" : 2,
    prefix2 : "raw" : 2,
    program_ref_kind : "structural" : 1,
    program_account_index : "structural" : 1,
    alt_address_index : "identity" : 1,
    alt_role : "structural" : 1,
    prefix_flags : "raw" : 1,
    prefix1 : "raw" : 1,
    head_len : "raw" : 1,
    data_len_class : "structural" : 1,
    position_pattern_complete : "structural" : 1,
    program_identity_available : "identity" : 1,
);

spec_mod!(
    account_cols,
    AccountRoleV1,
    identity_hash64 : "identity" : 8,
    alt_identity_hash64 : "identity" : 8,
    static_pubkey_offset : "raw" : 4,
    alt_table_pubkey_offset : "raw" : 4,
    expanded_index : "structural" : 2,
    static_index : "structural" : 2,
    alt_table_ordinal : "identity" : 2,
    usage_count : "structural" : 2,
    writable_usage_count : "structural" : 2,
    program_usage_count : "structural" : 2,
    first_instruction_seen : "structural" : 2,
    last_instruction_seen : "structural" : 2,
    origin : "structural" : 1,
    signer : "structural" : 1,
    writable : "structural" : 1,
    readonly : "structural" : 1,
    fee_payer : "structural" : 1,
    used_as_program : "structural" : 1,
    used_as_instruction_account : "structural" : 1,
    alt_address_index : "identity" : 1,
    has_static_identity : "identity" : 1,
    static_pubkey_len : "raw" : 1,
    alt_table_pubkey_len : "raw" : 1,
);

struct Lists {
    ix: Vec<ListBuilder<UInt64Builder>>,
    ix_head: ListBuilder<FixedSizeBinaryBuilder>,
    ix_prefix16: ListBuilder<FixedSizeBinaryBuilder>,
    account: Vec<ListBuilder<UInt64Builder>>,
}

impl Lists {
    fn new() -> Self {
        Self {
            ix: (0..ix_cols::SPECS.len())
                .map(|_| ListBuilder::new(UInt64Builder::new()))
                .collect(),
            ix_head: ListBuilder::new(FixedSizeBinaryBuilder::with_capacity(0, 32)),
            ix_prefix16: ListBuilder::new(FixedSizeBinaryBuilder::with_capacity(0, 16)),
            account: (0..account_cols::SPECS.len())
                .map(|_| ListBuilder::new(UInt64Builder::new()))
                .collect(),
        }
    }

    fn push_ok(&mut self, out: &EncodedTxV1) {
        let n = out.header.instruction_count as usize;
        let m = out.header.total_account_reference_count as usize;
        for ix in &out.instructions[..n] {
            for (builder, spec) in self.ix.iter_mut().zip(ix_cols::SPECS.iter()) {
                builder.values().append_value((spec.get)(ix));
            }
            self.ix_head.values().append_value(ix.head.as_slice()).expect("head");
            self.ix_prefix16
                .values()
                .append_value(ix.prefix16.as_slice())
                .expect("prefix16");
        }
        for builder in &mut self.ix {
            builder.append(true);
        }
        self.ix_head.append(true);
        self.ix_prefix16.append(true);
        for acct in &out.accounts[..m] {
            for (builder, spec) in self.account.iter_mut().zip(account_cols::SPECS.iter()) {
                builder.values().append_value((spec.get)(acct));
            }
        }
        for builder in &mut self.account {
            builder.append(true);
        }
    }

    fn push_empty(&mut self) {
        for builder in &mut self.ix {
            builder.append(true);
        }
        self.ix_head.append(true);
        self.ix_prefix16.append(true);
        for builder in &mut self.account {
            builder.append(true);
        }
    }
}

fn tagged(name: &str, dtype: DataType, group: &str, source_width: Option<u8>) -> Field {
    let mut meta = std::collections::HashMap::new();
    meta.insert("group".into(), group.into());
    meta.insert("feature_schema".into(), FEATURE_SCHEMA.into());
    if let Some(width) = source_width {
        meta.insert("source_width".into(), width.to_string());
    }
    Field::new(name, dtype, false).with_metadata(meta)
}

fn schema() -> Schema {
    let mut fields = vec![
        tagged("slot", DataType::UInt64, "join", None),
        tagged("tx_index", DataType::UInt32, "join", None),
        tagged("signature", DataType::Utf8, "join", None),
        tagged("feature_schema", DataType::Utf8, "meta", None),
        tagged("encode_status", DataType::Int32, "meta", None),
        tagged("encode_status_name", DataType::Utf8, "meta", None),
    ];
    for spec in header_cols::SPECS {
        let dtype = match spec.width {
            1 => DataType::UInt8,
            2 => DataType::UInt16,
            4 => DataType::UInt32,
            _ => DataType::UInt64,
        };
        fields.push(tagged(spec.name, dtype, spec.group, None));
    }
    let ix_item = DataType::List(Arc::new(Field::new("item", DataType::UInt64, true)));
    for spec in ix_cols::SPECS {
        fields.push(tagged(
            &format!("ix_{}", spec.name),
            ix_item.clone(),
            spec.group,
            Some(spec.width),
        ));
    }
    fields.push(tagged(
        "ix_head",
        DataType::List(Arc::new(Field::new("item", DataType::FixedSizeBinary(32), true))),
        "raw",
        None,
    ));
    fields.push(tagged(
        "ix_prefix16",
        DataType::List(Arc::new(Field::new("item", DataType::FixedSizeBinary(16), true))),
        "raw",
        None,
    ));
    for spec in account_cols::SPECS {
        fields.push(tagged(
            &format!("acct_{}", spec.name),
            ix_item.clone(),
            spec.group,
            Some(spec.width),
        ));
    }
    let mut metadata = std::collections::HashMap::new();
    metadata.insert("feature_schema".into(), FEATURE_SCHEMA.into());
    metadata.insert(
        "note".into(),
        "raw transaction bytes stay in the master JSONL; this file is features plus join keys".into(),
    );
    Schema::new_with_metadata(fields, metadata)
}

fn widen(width: u8, values: &[u64]) -> ArrayRef {
    match width {
        1 => Arc::new(UInt8Array::from_iter_values(values.iter().map(|v| *v as u8))),
        2 => Arc::new(UInt16Array::from_iter_values(values.iter().map(|v| *v as u16))),
        4 => Arc::new(UInt32Array::from_iter_values(values.iter().map(|v| *v as u32))),
        _ => Arc::new(UInt64Array::from_iter_values(values.iter().copied())),
    }
}

struct Batch {
    slot: Vec<u64>,
    tx_index: Vec<u32>,
    signature: Vec<String>,
    status: Vec<i32>,
    status_name: Vec<String>,
    header: Vec<Vec<u64>>,
    lists: Lists,
}

impl Batch {
    fn new() -> Self {
        Self {
            slot: Vec::with_capacity(BATCH),
            tx_index: Vec::with_capacity(BATCH),
            signature: Vec::with_capacity(BATCH),
            status: Vec::with_capacity(BATCH),
            status_name: Vec::with_capacity(BATCH),
            header: header_cols::SPECS.iter().map(|_| Vec::with_capacity(BATCH)).collect(),
            lists: Lists::new(),
        }
    }

    fn len(&self) -> usize {
        self.slot.len()
    }

    fn push(&mut self, slot: u64, tx_index: u32, signature: &str, status: Status, out: &EncodedTxV1) {
        self.slot.push(slot);
        self.tx_index.push(tx_index);
        self.signature.push(signature.to_string());
        self.status.push(status.as_i32());
        self.status_name.push(status.name().to_string());
        for (col, spec) in self.header.iter_mut().zip(header_cols::SPECS.iter()) {
            let value = if status == Status::Ok {
                (spec.get)(&out.header)
            } else {
                0
            };
            col.push(value);
        }
        if status == Status::Ok {
            self.lists.push_ok(out);
        } else {
            self.lists.push_empty();
        }
    }

    fn finish(&mut self, schema: &Schema) -> RecordBatch {
        let mut cols: Vec<ArrayRef> = Vec::new();
        cols.push(Arc::new(UInt64Array::from(std::mem::take(&mut self.slot))));
        cols.push(Arc::new(UInt32Array::from(std::mem::take(&mut self.tx_index))));
        cols.push(Arc::new(StringArray::from(std::mem::take(&mut self.signature))));
        let n = self.status.len();
        cols.push(Arc::new(StringArray::from(vec![FEATURE_SCHEMA; n])));
        cols.push(Arc::new(Int32Array::from(std::mem::take(&mut self.status))));
        cols.push(Arc::new(StringArray::from(std::mem::take(&mut self.status_name))));
        for (col, spec) in self.header.iter_mut().zip(header_cols::SPECS.iter()) {
            cols.push(widen(spec.width, col));
            col.clear();
        }
        for builder in &mut self.lists.ix {
            cols.push(Arc::new(builder.finish()));
        }
        cols.push(Arc::new(self.lists.ix_head.finish()));
        cols.push(Arc::new(self.lists.ix_prefix16.finish()));
        for builder in &mut self.lists.account {
            cols.push(Arc::new(builder.finish()));
        }
        RecordBatch::try_new(Arc::new(schema.clone()), cols).expect("feature batch schema")
    }
}

pub fn encode_jsonl(input: &Path, output: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let text = std::fs::read_to_string(input)?;
    let schema = schema();
    let props = WriterProperties::builder()
        .set_compression(Compression::SNAPPY)
        .build();
    let file = File::create(output)?;
    let mut writer = ArrowWriter::try_new(file, Arc::new(schema.clone()), Some(props))?;
    let mut scratch = Scratch::new();
    let mut out = EncodedTxV1::boxed();
    let mut batch = Batch::new();
    let mut rows = 0u64;
    let mut ok = 0u64;
    let mut failed = 0u64;

    for (line_no, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let row: Row = serde_json::from_str(line)
            .map_err(|err| format!("{}:{}: {err}", input.display(), line_no + 1))?;
        let raw = decode_hex(&row.raw_hex)
            .map_err(|err| format!("{}:{}: {err}", input.display(), line_no + 1))?;
        if let Some(declared) = row.raw_len {
            if declared != raw.len() as u64 {
                return Err(format!(
                    "{}:{}: raw_len {} != decoded {}",
                    input.display(),
                    line_no + 1,
                    declared,
                    raw.len()
                )
                .into());
            }
        }
        let status = encode_transaction_v1(&raw, &mut scratch, &mut out);
        if status == Status::Ok {
            ok += 1;
        } else {
            failed += 1;
        }
        batch.push(row.slot, row.tx_index, &row.signature, status, &out);
        rows += 1;
        if batch.len() >= BATCH {
            writer.write(&batch.finish(&schema))?;
        }
    }
    if batch.len() > 0 {
        writer.write(&batch.finish(&schema))?;
    } else if rows == 0 {
        writer.write(&RecordBatch::new_empty(Arc::new(schema)))?;
    }
    writer.close()?;
    eprintln!(
        "encode-jsonl rows={rows} ok={ok} encode_failed={failed} schema={FEATURE_SCHEMA} output={}",
        output.display()
    );
    Ok(())
}

#[derive(serde::Deserialize)]
struct Row {
    slot: u64,
    tx_index: u32,
    signature: String,
    raw_hex: String,
    raw_len: Option<u64>,
}

fn decode_hex(s: &str) -> Result<Vec<u8>, String> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return Err("odd hex length".into());
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    for chunk in b.chunks(2) {
        let hi = hex_val(chunk[0])?;
        let lo = hex_val(chunk[1])?;
        out.push((hi << 4) | lo);
    }
    Ok(out)
}

fn hex_val(c: u8) -> Result<u8, String> {
    match c {
        b'0'..=b'9' => Ok(c - b'0'),
        b'a'..=b'f' => Ok(c - b'a' + 10),
        b'A'..=b'F' => Ok(c - b'A' + 10),
        _ => Err(format!("bad hex byte {c}")),
    }
}
