mod bench_cmd;
mod counting_alloc;
mod encode_cmd;
mod join_cmd;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use counting_alloc::CountingAlloc;

#[global_allocator]
static GLOBAL: CountingAlloc = CountingAlloc;

#[derive(Parser)]
#[command(name = "tx-features", about = "framed-tx-v1 historical tools")]
struct Cli {
    #[command(subcommand)]
    cmd: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Hex-decode JSONL transactions and write framed-tx-v1 Parquet features.
    EncodeJsonl {
        #[arg(long)]
        input: PathBuf,
        #[arg(long)]
        output: PathBuf,
    },
    /// Left-join a generic label JSONL onto feature Parquet by a key column.
    JoinLabels {
        #[arg(long)]
        features: PathBuf,
        #[arg(long)]
        labels: PathBuf,
        #[arg(long)]
        output: PathBuf,
        #[arg(long, default_value = "signature")]
        on: String,
    },
    /// Time encode_transaction_v1 on a JSONL corpus and write a markdown report.
    Bench {
        #[arg(long)]
        input: PathBuf,
        #[arg(long)]
        output: PathBuf,
        #[arg(long, default_value_t = 20_000)]
        samples: u32,
        #[arg(long, default_value_t = 50_000)]
        batch_iters: u32,
    },
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let result = match cli.cmd {
        Command::EncodeJsonl { input, output } => encode_cmd::encode_jsonl(&input, &output),
        Command::JoinLabels {
            features,
            labels,
            output,
            on,
        } => join_cmd::join_labels(&features, &labels, &output, &on),
        Command::Bench {
            input,
            output,
            samples,
            batch_iters,
        } => bench_cmd::bench(&input, &output, samples, batch_iters),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("error: {err}");
            ExitCode::from(1)
        }
    }
}
