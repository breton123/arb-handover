use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use ml_prep::build::{self, BuildArgs};
use ml_prep::raw::RawSource;

#[derive(Parser)]
#[command(name = "ml-prep", about = "Build ML datasets from raw transactions, economics labels and a candidate pack catalog")]
struct Cli {
    #[command(subcommand)]
    cmd: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Build tx_labels, tx_features, pairs, pack features and manifests.
    Build {
        /// Economics `labels` directory (trigger_occurrences, route_catalog, markets, execution_catalog).
        #[arg(long)]
        labels: PathBuf,
        /// `candidate-packs index-library` output for the same labels (positives.parquet).
        #[arg(long)]
        positives: PathBuf,
        /// Frozen pack catalog: a `build-library` directory or a legacy `build` directory.
        #[arg(long)]
        packs: PathBuf,
        /// Directory of Old Faithful `{slot}.entries.json.zst` files.
        #[arg(long, conflicts_with_all = ["raw_jsonl", "raw_dataset"], required_unless_present_any = ["raw_jsonl", "raw_dataset"])]
        raw_entries: Option<PathBuf>,
        /// Fleet dataset partition (`dataset/` with compaction.json and part-*.transactions.parquet).
        #[arg(long, conflicts_with = "raw_jsonl")]
        raw_dataset: Option<PathBuf>,
        /// raw-tx JSONL (slot, tx_index, signature, raw_hex). Development only.
        #[arg(long)]
        raw_jsonl: Option<PathBuf>,
        #[arg(long)]
        out: PathBuf,
        #[arg(long, default_value_t = 250)]
        slots_per_part: u64,
        #[arg(long)]
        min_slot: Option<u64>,
        #[arg(long)]
        max_slot: Option<u64>,
        /// Emit pairs for packs with catalog rank below this depth.
        #[arg(long, default_value_t = 64)]
        max_depth: u32,
        /// Parts per million of UNKNOWN transactions that get live pairs when relevant.
        #[arg(long, default_value_t = 125_000)]
        unknown_pair_ppm: u32,
        #[arg(long, default_value_t = 8)]
        threads: usize,
        /// Continue a build in --out with identical inputs; finished parts are kept.
        #[arg(long)]
        resume: bool,
        /// JSON files to embed in the manifest (run.json, library sweeps, ...).
        #[arg(long)]
        provenance: Vec<PathBuf>,
        #[arg(long, default_value = "")]
        dataset_id: String,
        /// Chronological partition index in the library's --economics list.
        #[arg(long)]
        partition_index: Option<u64>,
        /// train_routes.parquet from `library-support` (RouteIds profitable in the library's train window).
        #[arg(long)]
        train_routes: Option<PathBuf>,
    },
    /// Write master_tx_id -> slot for one labels directory.
    Masters {
        #[arg(long)]
        labels: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    /// Route metadata for every library pack route and the train-window RouteId set, across partitions.
    LibrarySupport {
        #[arg(long)]
        library: PathBuf,
        /// Index directories in the library's chronological --economics order.
        #[arg(long, num_args = 1.., required = true)]
        index: Vec<PathBuf>,
        #[arg(long)]
        train_min: u64,
        #[arg(long)]
        train_max: u64,
        #[arg(long)]
        out: PathBuf,
    },
}

fn main() -> ExitCode {
    let result: ml_prep::Result<()> = match Cli::parse().cmd {
        Command::Masters { labels, out } => ml_prep::support::masters(&labels.join("trigger_occurrences.parquet"), &out)
            .map(|n| eprintln!("masters: {n} -> {}", out.display())),
        Command::LibrarySupport { library, index, train_min, train_max, out } => {
            ml_prep::support::library_support(&library, &index, (train_min, train_max), &out)
                .map(|r| println!("{}", serde_json::to_string(&r).unwrap()))
        }
        Command::Build {
            labels, positives, packs, raw_entries, raw_dataset, raw_jsonl, out, slots_per_part, min_slot, max_slot,
            max_depth, unknown_pair_ppm, threads, resume, provenance, dataset_id, partition_index, train_routes,
        } => build_command(BuildCli {
            labels, positives, packs, raw_entries, raw_dataset, raw_jsonl, out, slots_per_part, min_slot, max_slot,
            max_depth, unknown_pair_ppm, threads, resume, provenance, dataset_id, partition_index, train_routes,
        }),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("error: {err}");
            ExitCode::from(1)
        }
    }
}

struct BuildCli {
    labels: PathBuf,
    positives: PathBuf,
    packs: PathBuf,
    raw_entries: Option<PathBuf>,
    raw_dataset: Option<PathBuf>,
    raw_jsonl: Option<PathBuf>,
    out: PathBuf,
    slots_per_part: u64,
    min_slot: Option<u64>,
    max_slot: Option<u64>,
    max_depth: u32,
    unknown_pair_ppm: u32,
    threads: usize,
    resume: bool,
    provenance: Vec<PathBuf>,
    dataset_id: String,
    partition_index: Option<u64>,
    train_routes: Option<PathBuf>,
}

fn build_command(c: BuildCli) -> ml_prep::Result<()> {
    let BuildCli {
        labels, positives, packs, raw_entries, raw_dataset, raw_jsonl, out, slots_per_part, min_slot, max_slot,
        max_depth, unknown_pair_ppm, threads, resume, provenance, dataset_id, partition_index, train_routes,
    } = c;
    let result = (|| {
        let raw = match (raw_entries, raw_dataset, raw_jsonl) {
            (Some(dir), None, None) => RawSource::entries(&dir)?,
            (None, Some(dir), None) => RawSource::dataset(&dir, &labels, 4 * threads.max(1))?,
            (None, None, Some(path)) => RawSource::jsonl(&path)?,
            _ => return Err("pass exactly one of --raw-entries / --raw-dataset / --raw-jsonl".into()),
        };
        if slots_per_part == 0 || max_depth == 0 || unknown_pair_ppm > 1_000_000 {
            return Err("--slots-per-part and --max-depth must be > 0; --unknown-pair-ppm <= 1000000".into());
        }
        build::run(&BuildArgs {
            labels, positives, packs, raw, out, slots_per_part, min_slot, max_slot, max_depth,
            unknown_pair_ppm, threads, resume, provenance, dataset_id, partition_index, train_routes,
        })
    })();
    result
}
