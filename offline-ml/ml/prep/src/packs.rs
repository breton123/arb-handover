//! Candidate pack catalogs: the eligibility index and the pack-feature table.
//!
//! Two catalog formats are read, auto-detected from the directory:
//!
//! - `candidate-library-v1` (`library_packs.parquet` + `library.json`) from
//!   `candidate-packs build-library`: up to `max_packs_per_key` ranked packs per
//!   trigger key, built from training-window POSITIVE occurrences only. Its
//!   ranking metadata (rank, selection gain, training coverage, novelty counts,
//!   route support) is computed from the training window and frozen with the
//!   library, so it is usable as `train_meta` features provided the library's
//!   training window precedes the evaluated window. The notebook checks that.
//! - legacy `candidate-pack-v1` (`candidate_packs.parquet`) from `candidate-packs
//!   build`. Its coverage/objective/penalty statistics are not read; `rank` is
//!   kept only as the `builder_rank` baseline.
//!
//! Pack targets are not computed here. They come from profitable RouteIds
//! (`positives`) in the labels pass.

use std::collections::{BTreeSet, HashMap};
use std::path::Path;
use std::sync::Arc;

use arrow_array::{ArrayRef, Int64Array, StringArray, UInt32Array};
use arrow_schema::{DataType, Schema};
use tx_features::identity_hash64;

use crate::flatten::Kind;
use crate::pq::{self, Cols};
use crate::Result;

pub const LEGACY_FEATURE_SCHEMA: &str = "packflat-v1";
pub const LIBRARY_FEATURE_SCHEMA: &str = "packflat-v2";
pub const LIBRARY_SCHEMA: &str = "candidate-library-v1";

/// Venue programs with their own leg counter. Others are counted in
/// `legs_other_program`. Extending this list is a new pack feature schema.
pub const VENUE_PROGRAMS: &[(&str, &str)] = &[
    ("orca_whirlpool", "whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc"),
    ("raydium_clmm", "CAMMCzo5YL8w4VFF8KVHrK22GGUsp5VTaW7grrKgrWqK"),
    ("meteora_dlmm", "LBUZKhRxPF3XUpBCjp4YzTKgLccjZhTSDM9YuVaPwxo"),
];
pub const VENUE_FAMILIES: &[&str] = &["CLMM", "DLMM"];
pub const DYNAMIC_KINDS: &[&str] =
    &["CLMM_TICK_ARRAY", "DLMM_BIN_ARRAY", "DLMM_OPTIONAL_BITMAP", "USER_TOKEN_ACCOUNTS"];
pub const LAYERS: &[&str] = &["TOP_LEVEL", "INNER", "ATTRIBUTED"];

pub struct Pack {
    pub pack_id: String,
    pub program_id: String,
    pub family_id: String,
    pub layer: String,
    pub base: String,
    /// The catalog's own static rank within its trigger key (0 = first).
    pub rank: u32,
    pub route_ids: BTreeSet<String>,
    pub account_set_id: String,
    pub features: Vec<i64>,
}

#[derive(Clone)]
pub struct PackFeatureSpec {
    pub name: String,
    pub kind: Kind,
    /// `static` (pack structure/resources) or `train_meta` (training-window ranking metadata).
    pub group: &'static str,
}

pub struct PackIndex {
    /// `candidate-library-v1` or `candidate-pack-v1`.
    pub catalog_schema: String,
    pub feature_schema: &'static str,
    pub specs: Vec<PackFeatureSpec>,
    /// Sorted by `pack_id`. A pack's index in this vec is its stable ordinal.
    pub packs: Vec<Pack>,
    /// Inclusive `master_tx_id` window the catalog was built from.
    pub train_window: Option<(u64, u64)>,
    exact: HashMap<(String, String, String, String), Vec<usize>>,
    any_base: HashMap<(String, String, String), Vec<usize>>,
}

impl PackIndex {
    /// Packs eligible for one trigger key, in catalog rank order.
    ///
    /// The key is `(program_id, family_id, layer, base)`. The labeller writes
    /// `base = ""` only on `NO_LABEL` rows (no labels row, so no base). Those
    /// rows match every base's packs for the same `(program, family, layer)`,
    /// which is what a live system without labels would do. Rows with a base
    /// use the exact key, as the pack builders do.
    pub fn eligible(&self, program: &str, family: &str, layer: &str, base: &str) -> &[usize] {
        let hit = if base.is_empty() {
            self.any_base.get(&(program.to_string(), family.to_string(), layer.to_string()))
        } else {
            self.exact
                .get(&(program.to_string(), family.to_string(), layer.to_string(), base.to_string()))
        };
        hit.map(|v| v.as_slice()).unwrap_or(&[])
    }

    pub fn is_live(&self, pack: usize) -> bool {
        self.packs[pack].layer == "TOP_LEVEL"
    }
}

fn split_list(s: &str) -> impl Iterator<Item = &str> {
    s.split(',').filter(|x| !x.is_empty())
}

/// `prefix8:414b3f4ceb5b5b88:len28:accounts19` -> (8, 28, 19).
fn parse_family(family: &str) -> (u64, u64, u64) {
    let (mut prefix, mut len, mut accounts) = (0, 0, 0);
    for part in family.split(':') {
        if let Some(v) = part.strip_prefix("prefix") {
            prefix = v.parse().unwrap_or(0);
        } else if let Some(v) = part.strip_prefix("len") {
            len = v.parse().unwrap_or(0);
        } else if let Some(v) = part.strip_prefix("accounts") {
            accounts = v.parse().unwrap_or(0);
        }
    }
    (prefix, len, accounts)
}

struct RouteMeta {
    hops: u64,
    programs: Vec<String>,
    families: Vec<String>,
}

fn read_routes(route_catalog: &Path, wanted: &BTreeSet<String>) -> Result<HashMap<String, RouteMeta>> {
    let mut routes = HashMap::new();
    let (reader, order) = pq::open(route_catalog, &["route_id", "hops", "program_ids", "economic_families"])?;
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        let ids = c.str(0)?;
        for i in 0..batch.num_rows() {
            let id = ids.value(i);
            if wanted.contains(id) {
                routes.insert(
                    id.to_string(),
                    RouteMeta {
                        hops: c.u8(1)?.value(i) as u64,
                        programs: split_list(c.str(2)?.value(i)).map(str::to_string).collect(),
                        families: split_list(c.str(3)?.value(i)).map(str::to_string).collect(),
                    },
                );
            }
        }
    }
    if let Some(missing) = wanted.iter().find(|r| !routes.contains_key(*r)) {
        return Err(format!("pack route {missing} is not in the route catalog").into());
    }
    Ok(routes)
}

/// Key and route-structure features shared by both schemas.
fn structural_specs() -> Vec<(String, Kind)> {
    let mut v: Vec<(String, Kind)> = vec![
        ("layer_code".into(), Kind::Categorical),
        ("program_hash".into(), Kind::Hash),
        ("family_hash".into(), Kind::Hash),
        ("family_prefix_bytes".into(), Kind::Numeric),
        ("family_data_len".into(), Kind::Numeric),
        ("family_account_count".into(), Kind::Numeric),
        ("route_count".into(), Kind::Numeric),
        ("hops_min".into(), Kind::Numeric),
        ("hops_max".into(), Kind::Numeric),
        ("hops_sum".into(), Kind::Numeric),
        ("routes_2hop".into(), Kind::Numeric),
        ("routes_3hop".into(), Kind::Numeric),
        ("routes_4hop".into(), Kind::Numeric),
        ("routes_5plus_hop".into(), Kind::Numeric),
        ("distinct_route_patterns".into(), Kind::Numeric),
    ];
    for f in VENUE_FAMILIES {
        v.push((format!("legs_{}", f.to_lowercase()), Kind::Numeric));
    }
    v.push(("legs_other_family".into(), Kind::Numeric));
    for (name, _) in VENUE_PROGRAMS {
        v.push((format!("legs_{name}"), Kind::Numeric));
    }
    v.push(("legs_other_program".into(), Kind::Numeric));
    v
}

fn structural_values(p: &Pack, routes: &HashMap<String, RouteMeta>) -> Vec<i64> {
    let rs: Vec<&RouteMeta> = p.route_ids.iter().map(|r| &routes[r]).collect();
    let (prefix, data_len, accounts) = parse_family(&p.family_id);
    let program_bytes = bs58::decode(&p.program_id).into_vec().unwrap_or_default();
    let layer_code = LAYERS.iter().position(|l| *l == p.layer).unwrap_or(LAYERS.len()) as u64;
    let hops: Vec<u64> = rs.iter().map(|r| r.hops).collect();
    let patterns: BTreeSet<String> = rs.iter().map(|r| r.families.join(">")).collect();
    let mut f: Vec<u64> = vec![
        layer_code,
        identity_hash64(&program_bytes),
        identity_hash64(p.family_id.as_bytes()),
        prefix,
        data_len,
        accounts,
        rs.len() as u64,
        hops.iter().copied().min().unwrap_or(0),
        hops.iter().copied().max().unwrap_or(0),
        hops.iter().sum(),
        hops.iter().filter(|h| **h == 2).count() as u64,
        hops.iter().filter(|h| **h == 3).count() as u64,
        hops.iter().filter(|h| **h == 4).count() as u64,
        hops.iter().filter(|h| **h >= 5).count() as u64,
        patterns.len() as u64,
    ];
    let legs_fam: Vec<&String> = rs.iter().flat_map(|r| r.families.iter()).collect();
    for fam in VENUE_FAMILIES {
        f.push(legs_fam.iter().filter(|x| x.as_str() == *fam).count() as u64);
    }
    f.push(legs_fam.iter().filter(|x| !VENUE_FAMILIES.contains(&x.as_str())).count() as u64);
    let legs_prog: Vec<&String> = rs.iter().flat_map(|r| r.programs.iter()).collect();
    for (_, id) in VENUE_PROGRAMS {
        f.push(legs_prog.iter().filter(|x| x.as_str() == *id).count() as u64);
    }
    f.push(legs_prog.iter().filter(|x| !VENUE_PROGRAMS.iter().any(|(_, id)| x.as_str() == *id)).count() as u64);
    // Hashes are u64 bit patterns; i64 storage keeps the bits.
    f.into_iter().map(|v| v as i64).collect()
}

fn specs(list: Vec<(String, Kind, &'static str)>) -> Vec<PackFeatureSpec> {
    list.into_iter()
        .map(|(name, kind, group)| PackFeatureSpec { name: format!("pack_{name}"), kind, group })
        .collect()
}

const LEGACY_COLS: &[&str] = &[
    "pack_id", "program_id", "family_id", "layer", "base", "rank", "route_ids", "account_set_id",
    "account_count", "writable_count", "writable_conservative_count", "dynamic_groups", "dynamic_count",
    "estimated_message_bytes", "message_known", "estimated_cu", "cu_known", "train_master_tx_id_min",
    "train_master_tx_id_max",
];

/// Library columns used. Everything except identity, routes and the training
/// window is a feature; see [`library_specs`].
const LIBRARY_COLS: &[&str] = &[
    "pack_id", "program_id", "family_id", "layer", "base", "rank", "route_ids", "account_set_id",
    "pool_ids", "venue_sequences", "account_count", "writable_known", "writable_conservative",
    "estimated_message_bytes", "estimated_cu", "dynamic_requirements", "selection_gain",
    "train_covered_txs", "train_new_txs", "train_cumulative_txs", "train_key_txs", "new_route_ids",
    "new_pools", "new_venue_sequences", "new_hop_counts", "shared_accounts", "route_support_min",
    "route_support_sum", "train_master_tx_id_min", "train_master_tx_id_max",
];

fn library_specs() -> Vec<PackFeatureSpec> {
    let mut v: Vec<(String, Kind, &'static str)> =
        structural_specs().into_iter().map(|(n, k)| (n, k, "static")).collect();
    for name in [
        "pool_count", "venue_sequence_count", "account_count", "writable_known", "writable_conservative",
        "estimated_message_bytes", "estimated_cu", "dynamic_requirements",
    ] {
        v.push((name.into(), Kind::Numeric, "static"));
    }
    for name in [
        "library_rank", "selection_gain", "train_covered_txs", "train_new_txs", "train_cumulative_txs",
        "train_key_txs", "new_route_ids", "new_pools", "new_venue_sequences", "new_hop_counts",
        "shared_accounts", "route_support_min", "route_support_sum",
    ] {
        v.push((name.into(), Kind::Numeric, "train_meta"));
    }
    specs(v)
}

fn legacy_specs() -> Vec<PackFeatureSpec> {
    let mut v: Vec<(String, Kind, &'static str)> =
        structural_specs().into_iter().map(|(n, k)| (n, k, "static")).collect();
    for name in ["account_count", "writable_count", "writable_conservative_count", "dynamic_count"] {
        v.push((name.into(), Kind::Numeric, "static"));
    }
    for d in DYNAMIC_KINDS {
        v.push((format!("dyn_{}", d.to_lowercase()), Kind::Numeric, "static"));
    }
    for name in ["dyn_other", "estimated_message_bytes", "message_known", "estimated_cu", "cu_known"] {
        v.push((name.into(), Kind::Numeric, "static"));
    }
    specs(v)
}

pub fn load(dir: &Path, route_catalog: &Path) -> Result<PackIndex> {
    if dir.join("library_packs.parquet").exists() {
        load_library(dir, route_catalog)
    } else {
        load_legacy(dir, route_catalog)
    }
}

fn load_library(dir: &Path, route_catalog: &Path) -> Result<PackIndex> {
    let manifest: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(dir.join("library.json"))?)?;
    if manifest["schema"] != LIBRARY_SCHEMA {
        return Err(format!("{}: unknown library schema {}", dir.display(), manifest["schema"]).into());
    }
    let (reader, order) = pq::open(&dir.join("library_packs.parquet"), LIBRARY_COLS)?;
    let mut packs = Vec::new();
    let mut windows = BTreeSet::new();
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        for i in 0..batch.num_rows() {
            let s = |j: usize| -> Result<String> { Ok(c.str(j)?.value(i).to_string()) };
            let u32v = |j: usize| -> Result<i64> { Ok(c.u32(j)?.value(i) as i64) };
            let u64v = |j: usize| -> Result<i64> { Ok(c.u64(j)?.value(i) as i64) };
            windows.insert((c.u64(28)?.value(i), c.u64(29)?.value(i)));
            let pools: BTreeSet<&str> = split_list(c.str(8)?.value(i)).collect();
            let sequences: BTreeSet<&str> = split_list(c.str(9)?.value(i)).collect();
            let rank = c.u32(5)?.value(i);
            let mut extra = vec![
                pools.len() as i64,
                sequences.len() as i64,
                u32v(10)?, u32v(11)?, u32v(12)?, u32v(13)?, u64v(14)?, u32v(15)?,
                rank as i64,
                pq::opt_i64(c.i64(16)?, i).unwrap_or(0),
            ];
            for j in 17..=27 {
                extra.push(u64v(j)?);
            }
            packs.push((
                Pack {
                    pack_id: s(0)?,
                    program_id: s(1)?,
                    family_id: s(2)?,
                    layer: s(3)?,
                    base: s(4)?,
                    rank,
                    route_ids: split_list(c.str(6)?.value(i)).map(str::to_string).collect(),
                    account_set_id: s(7)?,
                    features: Vec::new(),
                },
                extra,
            ));
        }
    }
    let wanted: BTreeSet<String> = packs.iter().flat_map(|(p, _)| p.route_ids.iter().cloned()).collect();
    let routes = read_routes(route_catalog, &wanted)?;
    let packs = packs
        .into_iter()
        .map(|(mut p, extra)| {
            let mut f = structural_values(&p, &routes);
            f.extend(extra);
            p.features = f;
            p
        })
        .collect();
    let train = (
        manifest["train_master_tx_id_min"].as_u64().ok_or("library.json: train_master_tx_id_min")?,
        manifest["train_master_tx_id_max"].as_u64().ok_or("library.json: train_master_tx_id_max")?,
    );
    if windows.iter().any(|w| *w != train) {
        return Err("library packs disagree with library.json on the training window".into());
    }
    finish(LIBRARY_SCHEMA.into(), LIBRARY_FEATURE_SCHEMA, library_specs(), packs, Some(train))
}

fn load_legacy(dir: &Path, route_catalog: &Path) -> Result<PackIndex> {
    let path = dir.join("candidate_packs.parquet");
    let (reader, order) = pq::open(&path, LEGACY_COLS)?;
    let mut packs = Vec::new();
    let mut windows = BTreeSet::new();
    for batch in reader {
        let batch = batch?;
        let c = Cols { batch: &batch, order: &order };
        for i in 0..batch.num_rows() {
            windows.insert((c.u64(17)?.value(i), c.u64(18)?.value(i)));
            let kinds: Vec<&str> =
                split_list(c.str(11)?.value(i)).map(|g| g.split(':').next().unwrap_or("")).collect();
            let mut extra: Vec<i64> = vec![
                c.u64(8)?.value(i) as i64,
                c.u64(9)?.value(i) as i64,
                c.u64(10)?.value(i) as i64,
                c.u64(12)?.value(i) as i64,
            ];
            for d in DYNAMIC_KINDS {
                extra.push(kinds.iter().filter(|k| *k == d).count() as i64);
            }
            extra.extend([
                kinds.iter().filter(|k| !DYNAMIC_KINDS.contains(k)).count() as i64,
                c.u64(13)?.value(i) as i64,
                c.bool(14)?.value(i) as i64,
                c.u64(15)?.value(i) as i64,
                c.bool(16)?.value(i) as i64,
            ]);
            packs.push((
                Pack {
                    pack_id: c.str(0)?.value(i).to_string(),
                    program_id: c.str(1)?.value(i).to_string(),
                    family_id: c.str(2)?.value(i).to_string(),
                    layer: c.str(3)?.value(i).to_string(),
                    base: c.str(4)?.value(i).to_string(),
                    rank: c.u32(5)?.value(i),
                    route_ids: split_list(c.str(6)?.value(i)).map(str::to_string).collect(),
                    account_set_id: c.str(7)?.value(i).to_string(),
                    features: Vec::new(),
                },
                extra,
            ));
        }
    }
    if windows.len() > 1 {
        return Err(format!("candidate packs carry {} different training windows", windows.len()).into());
    }
    let wanted: BTreeSet<String> = packs.iter().flat_map(|(p, _)| p.route_ids.iter().cloned()).collect();
    let routes = read_routes(route_catalog, &wanted)?;
    let packs = packs
        .into_iter()
        .map(|(mut p, extra)| {
            let mut f = structural_values(&p, &routes);
            f.extend(extra);
            p.features = f;
            p
        })
        .collect();
    finish("candidate-pack-v1".into(), LEGACY_FEATURE_SCHEMA, legacy_specs(), packs, windows.into_iter().next())
}

fn finish(
    catalog_schema: String,
    feature_schema: &'static str,
    specs: Vec<PackFeatureSpec>,
    mut packs: Vec<Pack>,
    train_window: Option<(u64, u64)>,
) -> Result<PackIndex> {
    packs.sort_by(|a, b| a.pack_id.cmp(&b.pack_id));
    for w in packs.windows(2) {
        if w[0].pack_id == w[1].pack_id {
            return Err(format!("duplicate pack_id {}", w[0].pack_id).into());
        }
    }
    for p in &packs {
        debug_assert_eq!(p.features.len(), specs.len());
        if p.features.len() != specs.len() {
            return Err(format!("pack {} has {} features, schema has {}", p.pack_id, p.features.len(), specs.len()).into());
        }
    }
    let mut exact: HashMap<(String, String, String, String), Vec<usize>> = HashMap::new();
    let mut any_base: HashMap<(String, String, String), Vec<usize>> = HashMap::new();
    for (i, p) in packs.iter().enumerate() {
        exact
            .entry((p.program_id.clone(), p.family_id.clone(), p.layer.clone(), p.base.clone()))
            .or_default()
            .push(i);
        any_base.entry((p.program_id.clone(), p.family_id.clone(), p.layer.clone())).or_default().push(i);
    }
    // Catalog rank order within each key (ties cannot occur within one key; pack_id breaks them anyway).
    for v in exact.values_mut().chain(any_base.values_mut()) {
        v.sort_by(|&a, &b| (packs[a].rank, &packs[a].pack_id).cmp(&(packs[b].rank, &packs[b].pack_id)));
    }
    Ok(PackIndex { catalog_schema, feature_schema, specs, packs, train_window, exact, any_base })
}

pub fn write_features(index: &PackIndex, path: &Path) -> Result<()> {
    let mut fields = vec![
        pq::field("pack_id", DataType::Utf8, false, "id", &[]),
        pq::field("pack_program_id", DataType::Utf8, false, "meta", &[]),
        pq::field("pack_family_id", DataType::Utf8, false, "meta", &[]),
        pq::field("pack_layer", DataType::Utf8, false, "meta", &[]),
        pq::field("pack_base", DataType::Utf8, false, "meta", &[]),
        pq::field("pack_account_set_id", DataType::Utf8, false, "meta", &[]),
        pq::field("pack_route_ids", DataType::Utf8, false, "meta", &[]),
        pq::field("builder_rank", DataType::UInt32, false, "baseline", &[]),
    ];
    for s in &index.specs {
        fields.push(pq::field(&s.name, DataType::Int64, false, "feature", &[("kind", s.kind.name()), ("group", s.group)]));
    }
    let schema = Arc::new(Schema::new_with_metadata(
        fields,
        [
            ("pack_feature_schema".to_string(), index.feature_schema.to_string()),
            ("catalog_schema".to_string(), index.catalog_schema.clone()),
        ]
        .into(),
    ));
    let p = &index.packs;
    let strs = |f: &dyn Fn(&Pack) -> String| -> ArrayRef { Arc::new(StringArray::from_iter_values(p.iter().map(f))) };
    let mut cols: Vec<ArrayRef> = vec![
        strs(&|x| x.pack_id.clone()),
        strs(&|x| x.program_id.clone()),
        strs(&|x| x.family_id.clone()),
        strs(&|x| x.layer.clone()),
        strs(&|x| x.base.clone()),
        strs(&|x| x.account_set_id.clone()),
        strs(&|x| x.route_ids.iter().cloned().collect::<Vec<_>>().join(",")),
        Arc::new(UInt32Array::from_iter_values(p.iter().map(|x| x.rank))),
    ];
    for j in 0..index.specs.len() {
        cols.push(Arc::new(Int64Array::from_iter_values(p.iter().map(|x| x.features[j]))));
    }
    pq::write_file(path, schema, cols)
}
