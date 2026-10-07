//! Release benchmark. Writes docs/ml/BENCHMARK_REPORT_V1.md.
//!
//! `cargo bench -p tx-features --bench encode`

use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use tx_features::bench_harness::{self, batch_encode, sample_encode, sample_overhead, warmup};
use tx_features::{
    copied_input_bytes, encode_transaction_v1, EncodedTxV1, Scratch, Status, FEATURE_SCHEMA,
    MAX_ACCOUNTS, MAX_INSTRUCTIONS, MAX_LOOKUP_TABLES,
};

static ARMED: AtomicBool = AtomicBool::new(false);
static COUNT: AtomicUsize = AtomicUsize::new(0);

struct CountingAlloc;
unsafe impl GlobalAlloc for CountingAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if ARMED.load(Ordering::Relaxed) {
            COUNT.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if ARMED.load(Ordering::Relaxed) {
            COUNT.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if ARMED.load(Ordering::Relaxed) {
            COUNT.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.realloc(ptr, layout, new_size) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static GLOBAL: CountingAlloc = CountingAlloc;

fn measure<R>(f: impl FnOnce() -> R) -> (R, usize) {
    COUNT.store(0, Ordering::Relaxed);
    ARMED.store(true, Ordering::Relaxed);
    let out = f();
    ARMED.store(false, Ordering::Relaxed);
    (out, COUNT.load(Ordering::Relaxed))
}

#[derive(Clone)]
struct TxMeasure {
    tx_index: u64,
    raw_len: u32,
    version: u8,
    instructions: u16,
    accounts: u16,
    lookups: u16,
    signatures: u16,
    uses_alt: u8,
    bytes_copied: u64,
    max_ix_data: u32,
    max_ix_accounts: u16,
    batch_ns: f64,
    batch_cycles: f64,
    allocs_per_tx: f64,
    sample_ns: Vec<f64>,
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Some(input) = arg_value(&args, "--input") {
        let output = arg_value(&args, "--output").unwrap_or_else(|| {
            concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/../../docs/ml/BENCHMARK_REPORT_100K.md"
            )
            .to_string()
        });
        run_corpus(&input, &output);
        return;
    }

    let samples: u32 = 20_000;
    let batch_iters: u32 = 50_000;
    let pinned = pin_thread();
    let hz = calibrate_hz();
    let overhead = {
        let mut buf = vec![0u64; 8192];
        sample_overhead(&mut buf);
        buf.sort_unstable();
        buf[buf.len() / 2]
    };
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../raw-tx-snippet.jsonl");
    let text = std::fs::read_to_string(path).expect("pilot jsonl");
    let mut scratch = Scratch::new();
    let mut out = EncodedTxV1::boxed();
    let mut rows = Vec::new();

    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let raw = decode_hex(&json_str(line, "raw_hex"));
        assert_eq!(
            encode_transaction_v1(&raw, &mut scratch, &mut out),
            Status::Ok
        );
        let bytes_copied = copied_input_bytes(&out);
        let max_ix_accounts = out.instructions[..out.header.instruction_count as usize]
            .iter()
            .map(|ix| ix.account_count)
            .max()
            .unwrap_or(0);
        warmup(&raw, &mut scratch, &mut out, 5_000);
        #[cfg(feature = "phase-profile")]
        for cell in &tx_features::PHASE_CYCLES {
            cell.store(0, std::sync::atomic::Ordering::Relaxed);
        }
        let ((ns, cycles), allocs) =
            measure(|| batch_encode(&raw, &mut scratch, &mut out, batch_iters as u64));
        #[cfg(feature = "phase-profile")]
        {
            let mut parts = [0u64; 4];
            for (i, cell) in tx_features::PHASE_CYCLES.iter().enumerate() {
                parts[i] = cell.load(std::sync::atomic::Ordering::Relaxed);
            }
            let sum = parts.iter().sum::<u64>().max(1);
            println!(
                "  phases cycles/tx parse={:.1} ({:.0}%) instructions={:.1} ({:.0}%) shared_scan={:.1} ({:.0}%) rollup_fp={:.1} ({:.0}%)",
                parts[0] as f64 / batch_iters as f64,
                100.0 * parts[0] as f64 / sum as f64,
                parts[1] as f64 / batch_iters as f64,
                100.0 * parts[1] as f64 / sum as f64,
                parts[2] as f64 / batch_iters as f64,
                100.0 * parts[2] as f64 / sum as f64,
                parts[3] as f64 / batch_iters as f64,
                100.0 * parts[3] as f64 / sum as f64,
            );
        }
        let mut sample_cycles = vec![0u64; samples as usize];
        let (_, sample_allocs) =
            measure(|| sample_encode(&raw, &mut scratch, &mut out, &mut sample_cycles));
        black_box(&out.header);
        let allocs_per_tx =
            (allocs as f64 / batch_iters as f64).max(sample_allocs as f64 / samples as f64);
        rows.push(TxMeasure {
            tx_index: json_u64(line, "tx_index"),
            raw_len: out.header.raw_len,
            version: out.header.version,
            instructions: out.header.instruction_count,
            accounts: out.header.total_account_reference_count,
            lookups: out.header.alt_lookup_count,
            signatures: out.header.signature_count,
            uses_alt: out.header.uses_alt,
            bytes_copied,
            max_ix_data: out.header.max_instruction_data_len,
            max_ix_accounts,
            batch_ns: ns as f64 / batch_iters as f64,
            batch_cycles: cycles as f64 / batch_iters as f64,
            allocs_per_tx,
            sample_ns: sample_cycles
                .into_iter()
                .map(|c| c.saturating_sub(overhead) as f64 / hz * 1e9)
                .collect(),
        });
        println!(
            "tx {} batch_ns={:.1} allocs/tx={:.3}",
            rows.last().unwrap().tx_index,
            rows.last().unwrap().batch_ns,
            rows.last().unwrap().allocs_per_tx
        );
    }

    #[cfg(feature = "phase-profile")]
    {
        println!("phase-profile build; not overwriting BENCHMARK_REPORT_V1.md");
        return;
    }
    #[cfg(not(feature = "phase-profile"))]
    {
        let report = render(&rows, hz, overhead, pinned, samples, batch_iters);
        let out_path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../docs/ml/BENCHMARK_REPORT_V1.md"
        );
        std::fs::write(out_path, &report).expect("write report");
        println!("wrote {out_path}");
    }
}

fn render(
    rows: &[TxMeasure],
    hz: f64,
    overhead_cycles: u64,
    pinned: bool,
    samples: u32,
    batch_iters: u32,
) -> String {
    let mut md = String::new();
    md.push_str("# framed-tx-v1 encode benchmark\n\n");
    md.push_str(&format!(
        "Schema `{FEATURE_SCHEMA}`. Corpus: `raw-tx-snippet.jsonl` ({} transactions).\n\n",
        rows.len()
    ));
    md.push_str("## Method\n\n");
    md.push_str("- `cargo bench -p tx-features --bench encode`. Profile inherits release: `opt-level=3`, thin LTO, `codegen-units=1`, `panic=unwind`.\n");
    md.push_str(&format!(
        "- Thread affinity pin: {}.\n",
        if pinned {
            "succeeded, mask bit 2, priority 2"
        } else {
            "not applied"
        }
    ));
    md.push_str(&format!("- TSC frequency from `rdtsc` over ~100 ms of wall time: {:.3} GHz. Empty `rdtsc` pair median: {overhead_cycles} cycles, subtracted from single-shot samples only.\n", hz / 1e9));
    md.push_str(&format!("- Each transaction: 5 000 untimed warmup calls, a tight batch of {batch_iters} calls, then {samples} single-shot samples.\n"));
    md.push_str("- The batch mean is the cost estimate. It has no per-call timer inside the loop. Single-shot percentiles still include residual measurement noise after the overhead subtraction.\n");
    md.push_str("- A counting allocator is armed only around the batch and the samples. Scratch, output, and the sample buffer are allocated before the counter is armed.\n");
    md.push_str("- Bytes copied are instruction data heads (at most 32 bytes each). Pubkeys are hashed in place.\n");
    md.push_str("- Figures are the measured values. They were not edited toward 1 µs.\n\n");

    md.push_str("## Observed pilot maxima\n\n");
    md.push_str("These are this file only. Capacities are the protocol bounds in `FRAMED_TX_FEATURES_V1.md`, not these maxima.\n\n");
    md.push_str("| quantity | observed max | V1 capacity |\n|---|---:|---:|\n");
    let max_of = |f: fn(&TxMeasure) -> u32| rows.iter().map(f).max().unwrap_or(0);
    md.push_str(&format!("| raw bytes | {} | 1232-byte packet is the current wire budget; the encoder accepts more until a table fills |\n", max_of(|r| r.raw_len)));
    md.push_str(&format!(
        "| instructions | {} | {MAX_INSTRUCTIONS} |\n",
        max_of(|r| r.instructions as u32)
    ));
    md.push_str(&format!(
        "| expanded accounts | {} | {MAX_ACCOUNTS} |\n",
        max_of(|r| r.accounts as u32)
    ));
    md.push_str(&format!(
        "| ALT lookups | {} | {MAX_LOOKUP_TABLES} |\n",
        max_of(|r| r.lookups as u32)
    ));
    md.push_str(&format!(
        "| signatures | {} | not stored |\n",
        max_of(|r| r.signatures as u32)
    ));
    md.push_str(&format!(
        "| max instruction data bytes | {} | shortvec u16 |\n",
        max_of(|r| r.max_ix_data)
    ));
    md.push_str(&format!(
        "| max accounts in one instruction | {} | shortvec u16 |\n\n",
        max_of(|r| r.max_ix_accounts as u32)
    ));

    md.push_str("## Per transaction\n\n");
    md.push_str("| tx | bytes | version | ixs | accounts | alt | batch ns | cycles/tx | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | allocs/tx | bytes copied |\n");
    md.push_str("|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for row in rows {
        let mut s = row.sample_ns.clone();
        s.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let ver = if row.version == 255 { "legacy" } else { "v0" };
        md.push_str(&format!(
            "| {} | {} | {ver} | {} | {} | {} | {:.1} | {:.1} | {:.1} | {:.1} | {:.1} | {:.1} | {:.0} | {:.3} | {} |\n",
            row.tx_index, row.raw_len, row.instructions, row.accounts, row.uses_alt,
            row.batch_ns, row.batch_cycles, pct(&s, 0.50), pct(&s, 0.90), pct(&s, 0.99), pct(&s, 0.999),
            1e9 / row.batch_ns, row.allocs_per_tx, row.bytes_copied
        ));
    }
    md.push('\n');
    md.push_str("## Buckets\n\n");
    md.push_str("p50–p99.9 pool every single-shot sample in the bucket. `batch ns` and `cycles/tx` are the unweighted mean of the per-transaction batch measurements. `tx/s` is `1e9 / batch ns`.\n\n");
    md.push_str("| bucket | txs | batch ns | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | cycles/tx | allocs/tx | bytes copied |\n");
    md.push_str("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    bucket(&mut md, "all", rows);
    bucket(&mut md, "legacy", &filter(rows, |r| r.version == 255));
    bucket(&mut md, "v0", &filter(rows, |r| r.version == 0));
    bucket(&mut md, "no ALT", &filter(rows, |r| r.uses_alt == 0));
    bucket(&mut md, "ALT", &filter(rows, |r| r.uses_alt == 1));
    bucket(&mut md, "bytes 0-399", &filter(rows, |r| r.raw_len < 400));
    bucket(
        &mut md,
        "bytes 400-799",
        &filter(rows, |r| (400..800).contains(&r.raw_len)),
    );
    bucket(&mut md, "bytes 800+", &filter(rows, |r| r.raw_len >= 800));
    bucket(
        &mut md,
        "ix 1-3",
        &filter(rows, |r| (1..4).contains(&r.instructions)),
    );
    bucket(
        &mut md,
        "ix 4-8",
        &filter(rows, |r| (4..9).contains(&r.instructions)),
    );
    bucket(&mut md, "ix 9+", &filter(rows, |r| r.instructions >= 9));
    md.push('\n');
    if !rows.is_empty() {
        let mean = rows.iter().map(|r| r.batch_ns).sum::<f64>() / rows.len() as f64;
        let fastest = rows.iter().map(|r| r.batch_ns).fold(f64::MAX, f64::min);
        let alloc_max = rows.iter().map(|r| r.allocs_per_tx).fold(0.0, f64::max);
        md.push_str("## Reading\n\n");
        md.push_str(&format!(
            "Aspirational target: batch mean under 1000 ns on common transactions. Across this pilot the mean batch time is {mean:.1} ns and the fastest transaction is {fastest:.1} ns. Maximum measured heap allocations per timed encode: {alloc_max:.3}.\n"
        ));
    }
    md
}

fn bucket(md: &mut String, name: &str, rows: &[TxMeasure]) {
    if rows.is_empty() {
        md.push_str(&format!("| {name} | 0 | | | | | | | | | |\n"));
        return;
    }
    let mut pooled = Vec::new();
    for row in rows {
        pooled.extend_from_slice(&row.sample_ns);
    }
    pooled.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let n = rows.len() as f64;
    let batch = rows.iter().map(|r| r.batch_ns).sum::<f64>() / n;
    let cycles = rows.iter().map(|r| r.batch_cycles).sum::<f64>() / n;
    let allocs = rows.iter().map(|r| r.allocs_per_tx).sum::<f64>() / n;
    let copied = rows.iter().map(|r| r.bytes_copied as f64).sum::<f64>() / n;
    md.push_str(&format!(
        "| {name} | {} | {batch:.1} | {:.1} | {:.1} | {:.1} | {:.1} | {:.0} | {cycles:.1} | {allocs:.3} | {copied:.1} |\n",
        rows.len(),
        pct(&pooled, 0.50),
        pct(&pooled, 0.90),
        pct(&pooled, 0.99),
        pct(&pooled, 0.999),
        if batch > 0.0 { 1e9 / batch } else { 0.0 }
    ));
}

fn filter(rows: &[TxMeasure], pred: impl Fn(&TxMeasure) -> bool) -> Vec<TxMeasure> {
    rows.iter().filter(|r| pred(r)).cloned().collect()
}

fn pct(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let idx = ((sorted.len() - 1) as f64 * p).round() as usize;
    sorted[idx.min(sorted.len() - 1)]
}

fn calibrate_hz() -> f64 {
    let start = Instant::now();
    let c0 = bench_harness::rdtsc();
    while start.elapsed() < Duration::from_millis(100) {
        black_box(bench_harness::rdtsc());
    }
    let cycles = bench_harness::rdtsc().wrapping_sub(c0) as f64;
    cycles / start.elapsed().as_nanos() as f64 * 1e9
}

fn pin_thread() -> bool {
    #[cfg(windows)]
    unsafe {
        extern "system" {
            fn GetCurrentThread() -> isize;
            fn SetThreadAffinityMask(thread: isize, mask: usize) -> usize;
            fn SetThreadPriority(thread: isize, priority: i32) -> i32;
        }
        let thread = GetCurrentThread();
        let affinity = SetThreadAffinityMask(thread, 1usize << 2);
        let _ = SetThreadPriority(thread, 2);
        affinity != 0
    }
    #[cfg(not(windows))]
    {
        false
    }
}

fn json_str(line: &str, key: &str) -> String {
    let pat = format!("\"{key}\": \"");
    let i = line.find(&pat).unwrap();
    let rest = &line[i + pat.len()..];
    rest[..rest.find('"').unwrap()].to_string()
}

fn json_u64(line: &str, key: &str) -> u64 {
    let pat = format!("\"{key}\": ");
    let i = line.find(&pat).unwrap();
    let rest = &line[i + pat.len()..];
    rest[..rest.find([',', '}']).unwrap()]
        .trim()
        .parse()
        .unwrap()
}

fn decode_hex(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    (0..b.len() / 2)
        .map(|i| (hex(b[i * 2]) << 4) | hex(b[i * 2 + 1]))
        .collect()
}

fn hex(c: u8) -> u8 {
    match c {
        b'0'..=b'9' => c - b'0',
        b'a'..=b'f' => c - b'a' + 10,
        _ => 0,
    }
}

fn arg_value(args: &[String], name: &str) -> Option<String> {
    args.iter()
        .position(|a| a == name)
        .and_then(|i| args.get(i + 1))
        .cloned()
}

struct CorpusTx {
    raw: Vec<u8>,
    raw_len: u32,
    version: u8,
    instructions: u16,
    accounts: u16,
    lookups: u16,
    signatures: u16,
    uses_alt: u8,
    bytes_copied: u32,
    max_ix_data: u32,
    max_ix_accounts: u16,
    batch_ns: f64,
    batch_cycles: f64,
    single_ns: f64,
}

fn run_corpus(input: &str, output: &str) {
    const REPEATS: u64 = 64;
    const PASSES: u64 = 8;
    let pinned = pin_thread();
    let hz = calibrate_hz();
    let overhead = {
        let mut buf = vec![0u64; 8192];
        sample_overhead(&mut buf);
        buf.sort_unstable();
        buf[buf.len() / 2]
    };
    eprintln!("loading {input}");
    let raws = load_raws(input);
    eprintln!("decoded {}", raws.len());
    let mut scratch = Scratch::new();
    let mut out = EncodedTxV1::boxed();
    let mut txs = Vec::with_capacity(raws.len());
    let mut status_counts = [0u64; 6];
    for raw in raws {
        let status = encode_transaction_v1(&raw, &mut scratch, &mut out);
        let slot = status.as_i32().clamp(0, 5) as usize;
        status_counts[slot] += 1;
        if status != Status::Ok {
            continue;
        }
        let h = &out.header;
        let max_ix_accounts = out.instructions[..h.instruction_count as usize]
            .iter()
            .map(|ix| ix.account_count)
            .max()
            .unwrap_or(0);
        txs.push(CorpusTx {
            raw,
            raw_len: h.raw_len,
            version: h.version,
            instructions: h.instruction_count,
            accounts: h.total_account_reference_count,
            lookups: h.alt_lookup_count,
            signatures: h.signature_count,
            uses_alt: h.uses_alt,
            bytes_copied: copied_input_bytes(&out) as u32,
            max_ix_data: h.max_instruction_data_len,
            max_ix_accounts,
            batch_ns: 0.0,
            batch_cycles: 0.0,
            single_ns: 0.0,
        });
    }
    eprintln!("ok {} / decoded, warming", txs.len());
    for _ in 0..2 {
        let mut sink = 0i32;
        for tx in &txs {
            sink ^= encode_transaction_v1(&tx.raw, &mut scratch, &mut out) as i32;
        }
        black_box(sink);
    }
    let (_, allocs) = measure(|| {
        let mut sink = 0i32;
        for tx in &txs {
            sink ^= encode_transaction_v1(black_box(&tx.raw), black_box(&mut scratch), black_box(&mut out))
                as i32;
        }
        black_box(sink);
    });
    let allocs_per_tx = if txs.is_empty() {
        0.0
    } else {
        allocs as f64 / txs.len() as f64
    };
    eprintln!("timing {REPEATS} repeats on each transaction");
    for (n, tx) in txs.iter_mut().enumerate() {
        let (ns, cycles) = batch_encode(&tx.raw, &mut scratch, &mut out, REPEATS);
        tx.batch_ns = ns as f64 / REPEATS as f64;
        tx.batch_cycles = cycles as f64 / REPEATS as f64;
        if (n + 1) % 20_000 == 0 {
            eprintln!("  batched {}", n + 1);
        }
    }
    eprintln!("single-shot pass");
    for tx in &mut txs {
        let c0 = bench_harness::rdtsc();
        let status = encode_transaction_v1(black_box(&tx.raw), black_box(&mut scratch), black_box(&mut out));
        let c1 = bench_harness::rdtsc();
        black_box(status);
        tx.single_ns = c1.wrapping_sub(c0).saturating_sub(overhead) as f64 / hz * 1e9;
    }
    eprintln!("{PASSES} full-corpus passes");
    let start = Instant::now();
    let c0 = bench_harness::rdtsc();
    let mut sink = 0i32;
    for _ in 0..PASSES {
        for tx in &txs {
            sink ^= encode_transaction_v1(black_box(&tx.raw), black_box(&mut scratch), black_box(&mut out))
                as i32;
        }
    }
    let pass_cycles = bench_harness::rdtsc().wrapping_sub(c0);
    let pass_ns = start.elapsed().as_nanos();
    black_box(sink);
    let timed = txs.len() as u128 * PASSES as u128;
    let corpus_ns = if timed == 0 {
        0.0
    } else {
        pass_ns as f64 / timed as f64
    };
    let corpus_cycles = if timed == 0 {
        0.0
    } else {
        pass_cycles as f64 / timed as f64
    };

    let report = render_corpus(
        &txs,
        &status_counts,
        input,
        hz,
        overhead,
        pinned,
        REPEATS,
        PASSES,
        corpus_ns,
        corpus_cycles,
        allocs_per_tx,
    );
    if let Some(parent) = std::path::Path::new(output).parent() {
        std::fs::create_dir_all(parent).ok();
    }
    std::fs::write(output, report).expect("write report");
    eprintln!("wrote {output}");
    eprintln!(
        "corpus {:.1} ns/tx  {:.0} tx/s  allocs/tx {allocs_per_tx:.3}  ok {}",
        corpus_ns,
        if corpus_ns > 0.0 { 1e9 / corpus_ns } else { 0.0 },
        txs.len()
    );
}

fn render_corpus(
    rows: &[CorpusTx],
    status_counts: &[u64; 6],
    input: &str,
    hz: f64,
    overhead_cycles: u64,
    pinned: bool,
    repeats: u64,
    passes: u64,
    corpus_ns: f64,
    corpus_cycles: f64,
    allocs_per_tx: f64,
) -> String {
    let mut md = String::new();
    md.push_str("# framed-tx-v1 encode benchmark, 100k corpus\n\n");
    md.push_str(&format!(
        "Schema `{FEATURE_SCHEMA}`. Input `{input}`.\n\n"
    ));
    md.push_str("## Method\n\n");
    md.push_str("- `cargo bench -p tx-features --bench encode --features bench-harness -- --input raw-tx-100k.jsonl`. Profile inherits release: `opt-level=3`, thin LTO, `codegen-units=1`, `panic=unwind`. `phase-profile` was off.\n");
    md.push_str(&format!(
        "- Thread affinity pin: {}.\n",
        if pinned {
            "succeeded, mask bit 2, priority 2"
        } else {
            "not applied"
        }
    ));
    md.push_str(&format!(
        "- TSC frequency from `rdtsc` over ~100 ms of wall time: {:.3} GHz. Empty `rdtsc` pair median: {overhead_cycles} cycles, subtracted from single-shot samples only.\n",
        hz / 1e9
    ));
    md.push_str("- Hex decode and JSONL parsing happen before the timed section. The timed calls see only raw transaction bytes, the same scratch, and the same output buffer.\n");
    md.push_str("- Two untimed warmup passes over every successful transaction.\n");
    md.push_str(&format!(
        "- Each successful transaction is then encoded {repeats} times in a tight loop. One timer covers those {repeats} calls. The tables below use that per-transaction mean.\n"
    ));
    md.push_str("- A separate single-shot pass records one `rdtsc` pair per transaction, minus the empty-pair median. Those percentiles still include residual timer noise.\n");
    md.push_str(&format!(
        "- Throughput is {passes} back-to-back passes over the successful corpus inside one timer. `ns/tx` and `cycles/tx` in the throughput line divide that one interval by the number of encodes.\n"
    ));
    md.push_str("- A counting allocator is armed around one extra full pass. Scratch, output, and the decoded transactions are allocated before that pass.\n");
    md.push_str("- Bytes copied are instruction data heads (at most 32 bytes each). Pubkey identity hashes copy 8-byte lanes on the stack and are not included.\n");
    md.push_str("- Transactions that do not return `OK` are counted and left out of the latency tables.\n");
    md.push_str("- Figures are the measured values. They were not edited toward 1 µs.\n\n");

    md.push_str("## Encode status\n\n");
    md.push_str("| status | count |\n|---|---:|\n");
    for (i, name) in ["OK", "MALFORMED", "UNSUPPORTED_VERSION", "CAPACITY_EXCEEDED", "NULL_ARGUMENT", "INTERNAL"]
        .iter()
        .enumerate()
    {
        md.push_str(&format!("| {name} | {} |\n", status_counts[i]));
    }
    md.push('\n');

    md.push_str("## Observed maxima\n\n");
    md.push_str("Successful encodes only. Capacities are the protocol bounds in `FRAMED_TX_FEATURES_V1.md`.\n\n");
    md.push_str("| quantity | observed max | V1 capacity |\n|---|---:|---:|\n");
    let max_of = |f: fn(&CorpusTx) -> u32| rows.iter().map(f).max().unwrap_or(0);
    md.push_str(&format!(
        "| raw bytes | {} | 1232-byte packet is the current wire budget; the encoder accepts more until a table fills |\n",
        max_of(|r| r.raw_len)
    ));
    md.push_str(&format!(
        "| instructions | {} | {MAX_INSTRUCTIONS} |\n",
        max_of(|r| r.instructions as u32)
    ));
    md.push_str(&format!(
        "| expanded accounts | {} | {MAX_ACCOUNTS} |\n",
        max_of(|r| r.accounts as u32)
    ));
    md.push_str(&format!(
        "| ALT lookups | {} | {MAX_LOOKUP_TABLES} |\n",
        max_of(|r| r.lookups as u32)
    ));
    md.push_str(&format!(
        "| signatures | {} | not stored |\n",
        max_of(|r| r.signatures as u32)
    ));
    md.push_str(&format!(
        "| max instruction data bytes | {} | shortvec u16 |\n",
        max_of(|r| r.max_ix_data)
    ));
    md.push_str(&format!(
        "| max accounts in one instruction | {} | shortvec u16 |\n\n",
        max_of(|r| r.max_ix_accounts as u32)
    ));

    md.push_str("## Throughput\n\n");
    md.push_str(&format!(
        "One timer around {passes} passes of {} successful transactions ({:.0} tx/s, {corpus_ns:.1} ns/tx, {corpus_cycles:.1} cycles/tx). Heap allocations on a separate full pass: {allocs_per_tx:.3} per encode.\n\n",
        rows.len(),
        if corpus_ns > 0.0 { 1e9 / corpus_ns } else { 0.0 }
    ));

    md.push_str("## Buckets\n\n");
    md.push_str(&format!(
        "Each row's latency is its {repeats}-call batch mean. p50–p99.9 are percentiles of those means across the transactions in the bucket. `batch ns` is their unweighted mean. `tx/s` is `1e9 / batch ns`. The throughput line above is the tighter measurement.\n\n"
    ));
    md.push_str("| bucket | txs | batch ns | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | cycles/tx | bytes copied |\n");
    md.push_str("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    corpus_bucket(&mut md, "all", rows, |_| true);
    corpus_bucket(&mut md, "legacy", rows, |r| r.version == 255);
    corpus_bucket(&mut md, "v0", rows, |r| r.version == 0);
    corpus_bucket(&mut md, "no ALT", rows, |r| r.uses_alt == 0);
    corpus_bucket(&mut md, "ALT", rows, |r| r.uses_alt == 1);
    corpus_bucket(&mut md, "bytes 0-399", rows, |r| r.raw_len < 400);
    corpus_bucket(&mut md, "bytes 400-799", rows, |r| (400..800).contains(&r.raw_len));
    corpus_bucket(&mut md, "bytes 800-1232", rows, |r| (800..1233).contains(&r.raw_len));
    corpus_bucket(&mut md, "bytes 1233+", rows, |r| r.raw_len >= 1233);
    corpus_bucket(&mut md, "ix 1-3", rows, |r| (1..4).contains(&r.instructions));
    corpus_bucket(&mut md, "ix 4-8", rows, |r| (4..9).contains(&r.instructions));
    corpus_bucket(&mut md, "ix 9-16", rows, |r| (9..17).contains(&r.instructions));
    corpus_bucket(&mut md, "ix 17+", rows, |r| r.instructions >= 17);
    md.push('\n');

    md.push_str("## Single-shot percentiles\n\n");
    md.push_str("One timed call per transaction. The empty `rdtsc` pair median is subtracted. Residual timer noise remains, so these sit above the batch means.\n\n");
    md.push_str("| bucket | txs | p50 ns | p90 ns | p99 ns | p99.9 ns |\n");
    md.push_str("|---|---:|---:|---:|---:|---:|\n");
    corpus_single(&mut md, "all", rows, |_| true);
    corpus_single(&mut md, "legacy", rows, |r| r.version == 255);
    corpus_single(&mut md, "v0", rows, |r| r.version == 0);
    corpus_single(&mut md, "no ALT", rows, |r| r.uses_alt == 0);
    corpus_single(&mut md, "ALT", rows, |r| r.uses_alt == 1);
    corpus_single(&mut md, "bytes 0-399", rows, |r| r.raw_len < 400);
    corpus_single(&mut md, "bytes 400-799", rows, |r| (400..800).contains(&r.raw_len));
    corpus_single(&mut md, "bytes 800+", rows, |r| r.raw_len >= 800);
    corpus_single(&mut md, "ix 1-3", rows, |r| (1..4).contains(&r.instructions));
    corpus_single(&mut md, "ix 4-8", rows, |r| (4..9).contains(&r.instructions));
    corpus_single(&mut md, "ix 9+", rows, |r| r.instructions >= 9);
    md.push('\n');

    if !rows.is_empty() {
        let mut means = rows.iter().map(|r| r.batch_ns).collect::<Vec<_>>();
        means.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let mean = means.iter().sum::<f64>() / means.len() as f64;
        md.push_str("## Reading\n\n");
        md.push_str(&format!(
            "Aspirational target for common transactions: under 1000 ns. The full-corpus throughput measurement is {corpus_ns:.1} ns/tx. The unweighted mean of the per-transaction {repeats}-call batches is {mean:.1} ns, with p50 {:.1} ns and p99 {:.1} ns. Heap allocations per encode on the counted pass: {allocs_per_tx:.3}.\n",
            pct(&means, 0.50),
            pct(&means, 0.99)
        ));
    }
    md
}

fn corpus_bucket(md: &mut String, name: &str, rows: &[CorpusTx], pred: impl Fn(&CorpusTx) -> bool) {
    let mut ns = Vec::new();
    let mut cycles = 0.0;
    let mut copied = 0.0;
    for row in rows.iter().filter(|r| pred(r)) {
        ns.push(row.batch_ns);
        cycles += row.batch_cycles;
        copied += row.bytes_copied as f64;
    }
    if ns.is_empty() {
        md.push_str(&format!("| {name} | 0 | | | | | | | | |\n"));
        return;
    }
    let n = ns.len() as f64;
    ns.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let batch = ns.iter().sum::<f64>() / n;
    md.push_str(&format!(
        "| {name} | {} | {batch:.1} | {:.1} | {:.1} | {:.1} | {:.1} | {:.0} | {:.1} | {:.1} |\n",
        ns.len(),
        pct(&ns, 0.50),
        pct(&ns, 0.90),
        pct(&ns, 0.99),
        pct(&ns, 0.999),
        if batch > 0.0 { 1e9 / batch } else { 0.0 },
        cycles / n,
        copied / n
    ));
}

fn corpus_single(md: &mut String, name: &str, rows: &[CorpusTx], pred: impl Fn(&CorpusTx) -> bool) {
    let mut ns: Vec<f64> = rows.iter().filter(|r| pred(r)).map(|r| r.single_ns).collect();
    if ns.is_empty() {
        md.push_str(&format!("| {name} | 0 | | | | |\n"));
        return;
    }
    ns.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    md.push_str(&format!(
        "| {name} | {} | {:.1} | {:.1} | {:.1} | {:.1} |\n",
        ns.len(),
        pct(&ns, 0.50),
        pct(&ns, 0.90),
        pct(&ns, 0.99),
        pct(&ns, 0.999)
    ));
}

fn load_raws(path: &str) -> Vec<Vec<u8>> {
    let text = std::fs::read_to_string(path).unwrap_or_else(|err| panic!("read {path}: {err}"));
    let mut raws = Vec::new();
    for (line_no, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let hex = json_raw_hex(line).unwrap_or_else(|| panic!("{}:{}: raw_hex", path, line_no + 1));
        raws.push(decode_hex_strict(hex, path, line_no + 1));
        if raws.len() % 20_000 == 0 {
            eprintln!("  decoded {}", raws.len());
        }
    }
    raws
}

fn json_raw_hex(line: &str) -> Option<&str> {
    let key = "\"raw_hex\"";
    let i = line.find(key)?;
    let rest = line[i + key.len()..].trim_start();
    let rest = rest.strip_prefix(':')?.trim_start();
    let rest = rest.strip_prefix('"')?;
    let end = rest.find('"')?;
    Some(&rest[..end])
}

fn decode_hex_strict(s: &str, path: &str, line_no: usize) -> Vec<u8> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        panic!("{path}:{line_no}: odd hex length");
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    for chunk in b.chunks(2) {
        out.push((hex_strict(chunk[0], path, line_no) << 4) | hex_strict(chunk[1], path, line_no));
    }
    out
}

fn hex_strict(c: u8, path: &str, line_no: usize) -> u8 {
    match c {
        b'0'..=b'9' => c - b'0',
        b'a'..=b'f' => c - b'a' + 10,
        b'A'..=b'F' => c - b'A' + 10,
        _ => panic!("{path}:{line_no}: bad hex"),
    }
}
