//! ML dataset preparation for the arb classifier and the candidate-pack ranker.
//!
//! Features come only from `tx_features::encode_transaction_v2` (framed-tx-v2),
//! flattened by [`flatten::flatten_v2`]. Labels and pack targets come from the
//! economics labeller and candidate-pack builder outputs, joined by
//! `master_tx_id` and checked against `(slot, tx_index, signature)`.

pub mod build;
pub mod dataset_source;
pub mod features;
pub mod flatten;
pub mod labels;
pub mod packs;
pub mod pairs;
pub mod pq;
pub mod raw;
pub mod relevance;
pub mod support;

pub type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
