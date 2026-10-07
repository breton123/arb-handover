//! `txflat-v2`: a fixed-width model row read from `EncodedTxV2` (framed-tx-v2).
//!
//! This is not a second encoder. Every value is copied from fields that
//! `encode_transaction_v2` already populated. Columns: the framed-tx header,
//! two aggregates, the v1-format message config, then the instruction slots. The only choice made here is
//! which instructions occupy the [`IX_SLOTS`] positional slots: the first
//! top-level instructions whose program is not Compute Budget, because Compute
//! Budget is already summarized in the header.
//!
//! [`flatten_v2`] does not allocate. Live inference calls it on the same
//! `EncodedTxV2` with the same feature order as training.

use std::sync::OnceLock;

use tx_features::{
    compute_budget_identity_hash64, EncodedTxV2, InstructionFeaturesV1, TxExtV2, TxHeaderV1, PROGRAM_ALT_REF,
    PROGRAM_STATIC,
};

pub const FLAT_SCHEMA: &str = "txflat-v2";
pub const IX_SLOTS: usize = 8;

/// How a training pipeline must treat the column.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Ordered integer. Used as a number.
    Numeric,
    /// 64-bit hash or fingerprint. Meaningless as a number; map through a
    /// train-fitted vocabulary.
    Hash,
    /// Small closed set of codes.
    Categorical,
}

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Kind::Numeric => "numeric",
            Kind::Hash => "hash",
            Kind::Categorical => "categorical",
        }
    }
}

#[derive(Clone, Debug)]
pub struct FeatureSpec {
    pub name: String,
    pub group: &'static str,
    pub kind: Kind,
    /// In-memory width in bytes of the stored integer: 1, 2, 4 or 8.
    pub width: u8,
}

type HeaderGet = fn(&TxHeaderV1) -> u64;
type IxGet = fn(&InstructionFeaturesV1) -> u64;

macro_rules! header_fields {
    ($($name:ident : $group:literal : $kind:ident : $width:literal),* $(,)?) => {
        static HEADER: &[(&str, &str, Kind, u8, HeaderGet)] = &[
            $((stringify!($name), $group, Kind::$kind, $width, |h: &TxHeaderV1| h.$name as u64),)*
        ];
    };
}

// Every header field marked "ML: yes" in FRAMED_TX_FEATURES_V1.md. `schema_id`
// is meta and is excluded.
header_fields!(
    version : "structural" : Categorical : 1,
    uses_alt : "structural" : Numeric : 1,
    raw_len : "structural" : Numeric : 4,
    signature_count : "structural" : Numeric : 2,
    required_signature_count : "structural" : Numeric : 2,
    readonly_signed_count : "structural" : Numeric : 2,
    readonly_unsigned_count : "structural" : Numeric : 2,
    static_account_count : "structural" : Numeric : 2,
    total_account_reference_count : "structural" : Numeric : 2,
    static_writable_count : "structural" : Numeric : 2,
    static_readonly_count : "structural" : Numeric : 2,
    static_signer_count : "structural" : Numeric : 2,
    instruction_count : "structural" : Numeric : 2,
    unique_program_reference_count : "structural" : Numeric : 2,
    total_instruction_account_refs : "structural" : Numeric : 4,
    unique_instruction_account_refs : "structural" : Numeric : 2,
    reused_account_ref_count : "structural" : Numeric : 4,
    total_instruction_data_bytes : "structural" : Numeric : 4,
    max_instruction_data_len : "structural" : Numeric : 4,
    min_instruction_data_len : "structural" : Numeric : 4,
    alt_lookup_count : "structural" : Numeric : 2,
    alt_loaded_writable_count : "structural" : Numeric : 2,
    alt_loaded_readonly_count : "structural" : Numeric : 2,
    compute_budget_instruction_count : "structural" : Numeric : 2,
    compute_budget_unknown_count : "structural" : Numeric : 2,
    compute_budget_malformed_count : "structural" : Numeric : 2,
    compute_budget_first_index : "structural" : Numeric : 2,
    compute_budget_last_index : "structural" : Numeric : 2,
    compute_budget_flags : "structural" : Numeric : 4,
    requested_cu_limit : "structural" : Numeric : 4,
    requested_cu_price : "structural" : Numeric : 8,
    requested_heap_bytes : "structural" : Numeric : 4,
    requested_loaded_accounts_data_size : "structural" : Numeric : 4,
    deprecated_requested_units : "structural" : Numeric : 4,
    deprecated_additional_fee : "structural" : Numeric : 4,
    max_account_usage_count : "structural" : Numeric : 2,
    max_writable_account_usage_count : "structural" : Numeric : 2,
    instructions_with_shared_accounts : "structural" : Numeric : 2,
    instructions_with_shared_writable_accounts : "structural" : Numeric : 2,
    accounts_used_by_multiple_instructions : "structural" : Numeric : 2,
    writable_accounts_used_by_multiple_instructions : "structural" : Numeric : 2,
    adjacent_pairs_sharing_account : "structural" : Numeric : 2,
    adjacent_pairs_sharing_writable : "structural" : Numeric : 2,
    program_repeated : "structural" : Numeric : 1,
    repeated_program_invocation_count : "structural" : Numeric : 2,
    program_sequence_fingerprint : "identity" : Hash : 8,
    instruction_structure_fingerprint : "structural" : Hash : 8,
    topology_fingerprint : "structural" : Hash : 8,
);

/// Program identity for one instruction: the static pubkey hash when the
/// program is a static key, otherwise the ALT descriptor hash.
fn program_key_hash(ix: &InstructionFeaturesV1) -> u64 {
    if ix.program_identity_available == 1 {
        ix.program_identity_hash64
    } else {
        ix.program_alt_identity_hash64
    }
}

static IX_FIELDS: &[(&str, &str, Kind, u8, IxGet)] = &[
    ("program_ref_kind", "structural", Kind::Categorical, 1, |i| i.program_ref_kind as u64),
    ("program_hash", "identity", Kind::Hash, 8, program_key_hash),
    ("data_len", "structural", Kind::Numeric, 4, |i| i.data_len as u64),
    ("data_len_class", "structural", Kind::Numeric, 1, |i| i.data_len_class as u64),
    ("account_count", "structural", Kind::Numeric, 2, |i| i.account_count as u64),
    ("signer_account_count", "structural", Kind::Numeric, 2, |i| i.signer_account_count as u64),
    ("writable_account_count", "structural", Kind::Numeric, 2, |i| i.writable_account_count as u64),
    ("alt_account_ref_count", "structural", Kind::Numeric, 2, |i| i.alt_account_ref_count as u64),
    ("reused_account_count", "structural", Kind::Numeric, 2, |i| i.reused_account_count as u64),
    ("prefix1", "raw", Kind::Numeric, 1, |i| i.prefix1 as u64),
    ("prefix8", "raw", Kind::Hash, 8, |i| i.prefix8),
    ("structural_family_hash", "structural", Kind::Hash, 8, |i| i.structural_family_hash64),
];

/// Columns after the header and before the instruction slots.
const AGGREGATES: &[(&str, &str, Kind, u8)] = &[
    ("non_cb_instruction_count", "structural", Kind::Numeric, 2),
    ("alt_program_instruction_count", "structural", Kind::Numeric, 2),
];

type ExtGet = fn(&TxExtV2) -> u64;

/// framed-tx-v2 v1-format message config. Zero for legacy and v0.
static EXT: &[(&str, &str, Kind, u8, ExtGet)] = &[
    ("config_mask", "structural", Kind::Numeric, 4, |e| e.config_mask as u64),
    ("config_priority_fee_lamports", "structural", Kind::Numeric, 8, |e| e.config_priority_fee_lamports),
    ("config_compute_unit_limit", "structural", Kind::Numeric, 4, |e| e.config_compute_unit_limit as u64),
    ("config_loaded_accounts_data_size", "structural", Kind::Numeric, 4, |e| e.config_loaded_accounts_data_size as u64),
    ("config_heap_size", "structural", Kind::Numeric, 4, |e| e.config_heap_size as u64),
];

/// Ordered feature list. Index `i` here is index `i` in [`flatten_v2`]'s output.
pub fn feature_specs() -> &'static [FeatureSpec] {
    static SPECS: OnceLock<Vec<FeatureSpec>> = OnceLock::new();
    SPECS.get_or_init(|| {
        let mut specs = Vec::new();
        for (name, group, kind, width, _) in HEADER {
            specs.push(FeatureSpec { name: name.to_string(), group, kind: *kind, width: *width });
        }
        for (name, group, kind, width) in AGGREGATES {
            specs.push(FeatureSpec { name: name.to_string(), group, kind: *kind, width: *width });
        }
        for (name, group, kind, width, _) in EXT {
            specs.push(FeatureSpec { name: name.to_string(), group, kind: *kind, width: *width });
        }
        for slot in 0..IX_SLOTS {
            specs.push(FeatureSpec {
                name: format!("ix{slot}_present"),
                group: "structural",
                kind: Kind::Numeric,
                width: 1,
            });
            for (name, group, kind, width, _) in IX_FIELDS {
                specs.push(FeatureSpec {
                    name: format!("ix{slot}_{name}"),
                    group,
                    kind: *kind,
                    width: *width,
                });
            }
        }
        specs
    })
}

pub fn feature_count() -> usize {
    HEADER.len() + AGGREGATES.len() + EXT.len() + IX_SLOTS * (1 + IX_FIELDS.len())
}

/// Fill `out` from an encoding that returned `Status::Ok`.
///
/// `cb_hash` is [`compute_budget_hash`]; it is passed in so the hot path does
/// not rehash the program id on every call.
pub fn flatten_v2(enc: &EncodedTxV2, cb_hash: u64, out: &mut [u64]) {
    assert_eq!(out.len(), feature_count(), "txflat-v2 row width");
    out.fill(0);
    let h = &enc.base.header;
    let mut k = 0;
    for (_, _, _, _, get) in HEADER {
        out[k] = get(h);
        k += 1;
    }
    let agg = k;
    k += AGGREGATES.len();
    for (_, _, _, _, get) in EXT {
        out[k] = get(&enc.ext);
        k += 1;
    }

    let n = h.instruction_count as usize;
    let mut non_cb = 0u64;
    let mut alt_programs = 0u64;
    let slot_width = 1 + IX_FIELDS.len();
    for ix in &enc.base.instructions[..n] {
        if ix.program_ref_kind == PROGRAM_ALT_REF {
            alt_programs += 1;
        }
        let is_cb = ix.program_ref_kind == PROGRAM_STATIC && ix.program_identity_hash64 == cb_hash;
        if is_cb {
            continue;
        }
        let slot = non_cb as usize;
        non_cb += 1;
        if slot >= IX_SLOTS {
            continue;
        }
        let base = k + slot * slot_width;
        out[base] = 1;
        for (j, (_, _, _, _, get)) in IX_FIELDS.iter().enumerate() {
            out[base + 1 + j] = get(ix);
        }
    }
    out[agg] = non_cb;
    out[agg + 1] = alt_programs;
}

pub fn compute_budget_hash() -> u64 {
    compute_budget_identity_hash64()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spec_count_matches_row_width() {
        assert_eq!(feature_specs().len(), feature_count());
        let mut names: Vec<&str> = feature_specs().iter().map(|s| s.name.as_str()).collect();
        names.sort();
        names.dedup();
        assert_eq!(names.len(), feature_count(), "feature names are unique");
    }

    #[test]
    fn no_identifier_or_label_names() {
        for spec in feature_specs() {
            for id in ["slot", "signature", "tx_index", "entry_index", "schema_id", "block_time"] {
                assert_ne!(spec.name, id, "{} is an identifier", spec.name);
            }
            for banned in ["master_tx", "label", "target", "pack_id", "profit", "route"] {
                assert!(!spec.name.contains(banned), "{} looks like an id/label", spec.name);
            }
        }
    }
}
