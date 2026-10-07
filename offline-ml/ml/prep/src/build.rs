//! `ml-prep build`: packs -> labels pass -> features pass -> pairs pass -> manifests.

use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow_array::{ArrayRef, StringArray, UInt64Array};
use arrow_schema::{DataType, Schema};
use rayon::prelude::*;
use serde::Serialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::features::{self, hex, PartReport};
use crate::flatten::{self, FLAT_SCHEMA, IX_SLOTS};
use crate::labels::{self, LabelSummary, Positives, TX_LABELS_SCHEMA};
use crate::packs::{self, PackIndex};
use crate::pairs::{self, PairReport, PAIRS_SCHEMA};
use crate::pq;
use crate::raw::RawSource;
use crate::relevance::{TrackedUniverse, MARKET_ROLES, RELEVANCE_SCHEMA};
use crate::Result;

pub const MANIFEST_SCHEMA: &str = "ml-prep-manifest-2";

pub struct BuildArgs {
    /// Economics labels directory: trigger_occurrences, route_catalog, markets, execution_catalog.
    pub labels: PathBuf,
    /// `candidate-packs index-library` output for the same labels (positives.parquet).
    pub positives: PathBuf,
    /// Candidate library (`build-library`) or legacy catalog (`build`) directory.
    pub packs: PathBuf,
    pub raw: RawSource,
    pub out: PathBuf,
    pub slots_per_part: u64,
    pub min_slot: Option<u64>,
    pub max_slot: Option<u64>,
    /// Pairs are emitted for packs with catalog rank < max_depth.
    pub max_depth: u32,
    /// Share (ppm) of UNKNOWN transactions that get live pairs when relevant.
    pub unknown_pair_ppm: u32,
    pub threads: usize,
    pub resume: bool,
    /// Extra JSON files (run manifests, acceptance, sweeps) copied into the manifest.
    pub provenance: Vec<PathBuf>,
    pub dataset_id: String,
    /// Chronological index of this partition in the library's `--economics`
    /// list (global id = index << 40 | master_tx_id). Recorded in the manifest.
    pub partition_index: Option<u64>,
    /// `train_routes.parquet` from `ml-prep library-support`.
    pub train_routes: Option<PathBuf>,
}

/// Catalog columns that are never features.
pub const EXCLUDED_PACK_COLUMNS: &[&str] = &[
    "legacy candidate_packs: objective_score, *_ppm, *_known, positive_covered/total, value_covered/total (rank only as builder_rank baseline)",
    "family_pack_policy.positive_prior_ppm",
    "library: train_master_tx_id_min/max, feasibility, config_sha256 (identity/meta only)",
];

pub fn sha256_file(path: &Path) -> Result<String> {
    let mut f = std::fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

#[derive(Serialize)]
struct InputRecord {
    path: String,
    bytes: u64,
    sha256: String,
}

fn input_record(path: &Path) -> Result<InputRecord> {
    Ok(InputRecord {
        path: path.display().to_string(),
        bytes: std::fs::metadata(path)?.len(),
        sha256: sha256_file(path)?,
    })
}

fn list_parts(dir: &Path) -> Result<Vec<String>> {
    let mut v: Vec<String> = std::fs::read_dir(dir)?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string())
        .filter(|n| n.starts_with("part-") && n.ends_with(".parquet"))
        .collect();
    v.sort();
    Ok(v)
}

/// Runs `job` once per part (in parallel), reusing a finished part on `--resume`.
fn per_part<R: Serialize + serde::de::DeserializeOwned + Send>(
    a: &BuildArgs,
    pool: &rayon::ThreadPool,
    parts: &[String],
    dir: &Path,
    stage: &str,
    job: impl Fn(&str, &Path) -> Result<R> + Sync,
) -> Result<Vec<R>> {
    std::fs::create_dir_all(dir)?;
    let done = std::sync::atomic::AtomicUsize::new(0);
    let reports: Vec<Result<R>> = pool.install(|| {
        parts
            .par_iter()
            .map(|name| {
                let out_part = dir.join(name);
                let sidecar = dir.join(format!("{name}.json"));
                let report = if a.resume && out_part.exists() && sidecar.exists() {
                    serde_json::from_str(&std::fs::read_to_string(&sidecar)?)?
                } else {
                    let r = job(name, &out_part)?;
                    pq::write_json(&sidecar, &r)?;
                    r
                };
                let k = done.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
                if k % 10 == 0 || k == parts.len() {
                    eprintln!("{stage}: {k}/{} parts", parts.len());
                }
                Ok(report)
            })
            .collect()
    });
    reports.into_iter().collect()
}

pub fn run(a: &BuildArgs) -> Result<()> {
    let started = std::time::Instant::now();
    std::fs::create_dir_all(&a.out)?;
    let trigger_occurrences = a.labels.join("trigger_occurrences.parquet");
    // Pack route metadata: the library's own `library_routes.parquet` (written by
    // `ml-prep library-support`, covers routes from every train partition) when
    // present, else this partition's labels route catalog.
    let route_catalog = if a.packs.join("library_routes.parquet").exists() {
        a.packs.join("library_routes.parquet")
    } else {
        a.labels.join("route_catalog.parquet")
    };
    let markets = a.labels.join("markets.parquet");
    let execution_catalog = a.labels.join("execution_catalog.parquet");
    let catalog_file = if a.packs.join("library_packs.parquet").exists() {
        a.packs.join("library_packs.parquet")
    } else {
        a.packs.join("candidate_packs.parquet")
    };

    // Inputs and config. A resumed build must see identical inputs.
    let mut inputs = BTreeMap::new();
    for (name, path) in [
        ("trigger_occurrences", trigger_occurrences.clone()),
        ("route_catalog", route_catalog.clone()),
        ("markets", markets.clone()),
        ("execution_catalog", execution_catalog.clone()),
        ("positives", a.positives.join("positives.parquet")),
        ("positives_index_manifest", a.positives.join("library-index.json")),
        ("pack_catalog", catalog_file.clone()),
    ] {
        inputs.insert(name.to_string(), input_record(&path)?);
    }
    if a.packs.join("library.json").exists() {
        inputs.insert("library_manifest".into(), input_record(&a.packs.join("library.json"))?);
    }
    if let RawSource::Jsonl { path, .. } = &a.raw {
        inputs.insert("raw_jsonl".to_string(), input_record(path)?);
    }
    if let Some(path) = &a.train_routes {
        inputs.insert("train_routes".to_string(), input_record(path)?);
    }
    let mut provenance = BTreeMap::new();
    for p in &a.provenance {
        let text = std::fs::read_to_string(p)?;
        let value: Value = serde_json::from_str(&text).unwrap_or(Value::String(text.clone()));
        provenance.insert(
            p.file_name().unwrap().to_string_lossy().to_string(),
            json!({ "path": p.display().to_string(), "sha256": sha256_file(p)?, "content": value }),
        );
    }
    let config = json!({
        "dataset_id": a.dataset_id,
        "slots_per_part": a.slots_per_part,
        "min_slot": a.min_slot,
        "max_slot": a.max_slot,
        "max_depth": a.max_depth,
        "unknown_pair_ppm": a.unknown_pair_ppm,
        "raw_source": { "kind": a.raw.kind(), "location": a.raw.describe() },
        "partition_index": a.partition_index,
        "global_id_shift": crate::support::PARTITION_SHIFT,
    });
    let input_digest = json!({
        "inputs": inputs.iter().map(|(k, v)| (k.clone(), v.sha256.clone())).collect::<BTreeMap<_, _>>(),
        "config": config,
        "schemas": [FLAT_SCHEMA, TX_LABELS_SCHEMA, PAIRS_SCHEMA, RELEVANCE_SCHEMA],
    });
    let digest_path = a.out.join("build-inputs.json");
    if digest_path.exists() {
        let prev: Value = serde_json::from_str(&std::fs::read_to_string(&digest_path)?)?;
        if !a.resume {
            return Err(format!("{} already holds a build; pass --resume or use a new --out", a.out.display()).into());
        }
        if prev != input_digest {
            return Err("inputs or config changed since the build in --out started; use a new --out".into());
        }
    }
    pq::write_json(&digest_path, &input_digest)?;

    // Packs and relevance universe.
    let pack_index: PackIndex = packs::load(&a.packs, &route_catalog)?;
    packs::write_features(&pack_index, &a.out.join("packs").join("pack_features.parquet"))?;
    eprintln!(
        "packs: {} {} packs, train window {:?}",
        pack_index.packs.len(),
        pack_index.catalog_schema,
        pack_index.train_window
    );
    let universe = TrackedUniverse::load(&markets, &execution_catalog)?;
    eprintln!("relevance: {} markets, {} market accounts", universe.markets, universe.market_account_count());

    // Labels pass.
    let summary_path = a.out.join("tx_labels").join("_summary.json");
    let summary: LabelSummary = if a.resume && summary_path.exists() && a.out.join("keys.parquet").exists() {
        serde_json::from_str(&std::fs::read_to_string(&summary_path)?)?
    } else {
        let positives = Positives::load(&a.positives)?;
        let train_routes = a.train_routes.as_deref().map(crate::support::read_route_set).transpose()?;
        let s = labels::run(
            &labels::LabelsArgs {
                trigger_occurrences: &trigger_occurrences,
                positives: &positives,
                out: &a.out,
                slots_per_part: a.slots_per_part,
                min_slot: a.min_slot,
                max_slot: a.max_slot,
                unknown_pair_ppm: a.unknown_pair_ppm,
                train_routes: train_routes.as_ref(),
            },
            &pack_index,
        )?;
        pq::write_json(&summary_path, &s)?;
        s
    };
    eprintln!(
        "labels: {} txs ({} positive, {} certified negative, {} unknown), {} keys, {} parts, {:.1}s",
        summary.transactions,
        summary.tx_positive,
        summary.tx_certified_negative,
        summary.tx_unknown,
        summary.trigger_keys,
        summary.parts.len(),
        started.elapsed().as_secs_f64()
    );
    let parts = list_parts(&a.out.join("tx_labels"))?;
    if parts != summary.parts {
        return Err("tx_labels parts on disk do not match the labels summary".into());
    }
    let pool = rayon::ThreadPoolBuilder::new().num_threads(a.threads).build()?;

    // Features pass.
    let labels_dir = a.out.join("tx_labels");
    let feat_dir = a.out.join("tx_features");
    let reports: Vec<PartReport> = per_part(a, &pool, &parts, &feat_dir, "features", |name, out| {
        features::build_part(&labels_dir.join(name), out, &a.raw, &universe)
    })?;

    // Pairs pass.
    let keys = labels::read_keys(&a.out.join("keys.parquet"))?;
    let pair_reports: Vec<PairReport> = per_part(a, &pool, &parts, &a.out.join("pairs"), "pairs", |name, out| {
        pairs::build_part(&labels_dir.join(name), &feat_dir.join(name), out, &keys, &pack_index, a.max_depth)
    })?;

    // Program dictionary for readable importances. Not a feature.
    let mut programs: BTreeMap<u64, String> = BTreeMap::new();
    for r in &reports {
        for (h, id) in &r.programs {
            programs.insert(u64::from_str_radix(h, 16)?, id.clone());
        }
    }
    let dict_schema = Arc::new(Schema::new(vec![
        pq::field("program_hash", DataType::UInt64, false, "dictionary", &[]),
        pq::field("program_id", DataType::Utf8, false, "dictionary", &[]),
    ]));
    let dict_cols: Vec<ArrayRef> = vec![
        Arc::new(UInt64Array::from_iter_values(programs.keys().copied())),
        Arc::new(StringArray::from_iter_values(programs.values())),
    ];
    pq::write_file(&a.out.join("dictionaries").join("programs.parquet"), dict_schema, dict_cols)?;

    write_feature_manifest(&a.out, &pack_index)?;

    // Output hashes and row counts, in path order.
    let mut rel_paths = vec![
        "packs/pack_features.parquet".to_string(),
        "dictionaries/programs.parquet".to_string(),
        "keys.parquet".to_string(),
        "feature_manifest.json".to_string(),
    ];
    for p in &parts {
        rel_paths.push(format!("tx_labels/{p}"));
        rel_paths.push(format!("tx_features/{p}"));
        rel_paths.push(format!("pairs/{p}"));
    }
    rel_paths.sort();
    let mut outputs = Vec::new();
    for rel in &rel_paths {
        let path = a.out.join(rel);
        let rows = if rel.ends_with(".parquet") { Some(pq::num_rows(&path)?) } else { None };
        outputs.push(json!({ "path": rel, "sha256": sha256_file(&path)?, "rows": rows }));
    }
    let mut raw_digest = Sha256::new();
    for r in &reports {
        raw_digest.update(r.raw_digest.as_bytes());
    }
    let feature_rows: u64 = reports.iter().map(|r| r.rows).sum();
    if feature_rows != summary.transactions {
        return Err(format!("feature rows {feature_rows} != tx_labels rows {}", summary.transactions).into());
    }
    let library_manifest: Value = if a.packs.join("library.json").exists() {
        serde_json::from_str(&std::fs::read_to_string(a.packs.join("library.json"))?)?
    } else {
        Value::Null
    };

    let manifest = json!({
        "manifest_schema": MANIFEST_SCHEMA,
        "tool": { "name": "ml-prep", "version": env!("CARGO_PKG_VERSION") },
        "schemas": {
            "tx_features": FLAT_SCHEMA,
            "encoder": tx_features::FEATURE_SCHEMA_V2,
            "tx_labels": TX_LABELS_SCHEMA,
            "pairs": PAIRS_SCHEMA,
            "pack_features": pack_index.feature_schema,
            "pack_catalog": pack_index.catalog_schema,
            "relevance": RELEVANCE_SCHEMA,
        },
        "config": config,
        "inputs": inputs,
        "raw_source": {
            "kind": a.raw.kind(),
            "location": a.raw.describe(),
            "slots_read": reports.iter().map(|r| r.slots).sum::<u64>(),
            "digest": hex(&raw_digest.finalize()),
        },
        "provenance": provenance,
        "labels": summary,
        "features": {
            "rows": feature_rows,
            "encode_ok": reports.iter().map(|r| r.encode_ok).sum::<u64>(),
            "encode_failed": reports.iter().map(|r| r.encode_failed).sum::<u64>(),
            "v1_format": reports.iter().map(|r| r.v1_format).sum::<u64>(),
            "relevant": reports.iter().map(|r| r.relevant).sum::<u64>(),
            "feature_count": flatten::feature_count(),
            "pack_feature_count": pack_index.specs.len(),
        },
        "pairs": {
            "rows": pair_reports.iter().map(|r| r.rows).sum::<u64>(),
            "positive_target": pair_reports.iter().map(|r| r.positive_target).sum::<u64>(),
            "transactions": pair_reports.iter().map(|r| r.transactions).sum::<u64>(),
            "max_depth": a.max_depth,
            "unknown_pair_ppm": a.unknown_pair_ppm,
            "rule": "POSITIVE: all-layer packs; CERTIFIED_NEGATIVE: live packs; UNKNOWN: live packs if relevant and pair_sample",
        },
        "packs": {
            "catalog_schema": pack_index.catalog_schema,
            "count": pack_index.packs.len(),
            "train_master_tx_id_min": pack_index.train_window.map(|w| w.0),
            "train_master_tx_id_max": pack_index.train_window.map(|w| w.1),
            "max_rank": pack_index.packs.iter().map(|p| p.rank).max(),
            "library": library_manifest,
        },
        "relevance": {
            "schema": RELEVANCE_SCHEMA,
            "rule": "relevant = a tracked market account or tracked venue program is a static account key of the transaction",
            "market_roles": MARKET_ROLES,
            "markets": universe.markets,
            "market_accounts": universe.market_account_count(),
            "universe_digest": universe.digest,
            "not_seen": "ALT-loaded addresses (not on the wire)",
        },
        "leakage": {
            "excluded_pack_columns": EXCLUDED_PACK_COLUMNS,
            "train_meta_pack_features": "library ranking metadata computed from the library's training window; valid only for windows after it",
            "live_eligibility": "eligible_live = pack key layer TOP_LEVEL. INNER/ATTRIBUTED keys come from execution metadata",
            "feature_files_have_labels": false,
        },
        "outputs": outputs,
    });
    pq::write_json(&a.out.join("manifest.json"), &manifest)?;
    pq::write_json(
        &a.out.join("timing.json"),
        &json!({ "wall_seconds": started.elapsed().as_secs_f64(), "threads": a.threads }),
    )?;
    eprintln!("done: {} in {:.1}s", a.out.display(), started.elapsed().as_secs_f64());
    Ok(())
}

pub fn write_feature_manifest(out: &Path, packs: &PackIndex) -> Result<()> {
    let tx: Vec<Value> = flatten::feature_specs()
        .iter()
        .map(|s| json!({ "name": s.name, "dtype": format!("uint{}", s.width as u32 * 8), "group": s.group, "kind": s.kind.name() }))
        .collect();
    let pack: Vec<Value> = packs
        .specs
        .iter()
        .map(|s| json!({ "name": s.name, "dtype": "int64", "group": s.group, "kind": s.kind.name() }))
        .collect();
    let manifest = json!({
        "feature_schema": FLAT_SCHEMA,
        "encoder_schema": tx_features::FEATURE_SCHEMA_V2,
        "pack_feature_schema": packs.feature_schema,
        "pack_catalog_schema": packs.catalog_schema,
        "relevance_schema": RELEVANCE_SCHEMA,
        "ix_slots": IX_SLOTS,
        "ix_slot_rule": "first top-level instructions whose program is not ComputeBudget, in order",
        "tx_features": tx,
        "pack_features": pack,
        "gate_columns": features::GATE_COLUMNS.iter().map(|(n, _)| *n).collect::<Vec<_>>(),
        "id_columns": ["master_tx_id", "slot", "entry_index", "tx_index", "signature", "pack_id"],
        "label_columns": [
            "arb_label", "arb_target", "label_quality", "unknown_reasons", "n_covering_live", "n_covering_oracle",
            "min_cover_rank_live", "min_cover_rank_oracle", "n_profitable_routes", "n_profitable_routes_seen_in_train",
            "profitable_routes", "best_net_profit", "pack_target"
        ],
        "meta_columns": [
            "block_time", "encode_status", "n_occurrences", "n_occ_top_level", "n_occ_inner", "n_occ_attributed",
            "trigger_keys", "n_eligible_live", "n_eligible_oracle", "pair_sample", "eligible_live", "pack_rank",
            "builder_rank", "pack_program_id", "pack_family_id", "pack_layer", "pack_base", "pack_account_set_id",
            "pack_route_ids"
        ],
    });
    pq::write_json(&out.join("feature_manifest.json"), &manifest)
}
