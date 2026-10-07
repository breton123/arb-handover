use std::path::Path;
use std::time::{Duration, Instant};

use tx_features::bench_harness::{self, batch_encode, sample_encode, sample_overhead, warmup};
use tx_features::{
    copied_input_bytes, encode_transaction_v1, EncodedTxV1, Scratch, Status, FEATURE_SCHEMA,
    MAX_ACCOUNTS, MAX_INSTRUCTIONS, MAX_LOOKUP_TABLES,
};

use crate::counting_alloc;

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

pub fn bench(
    input: &Path,
    output: &Path,
    samples: u32,
    batch_iters: u32,
) -> Result<(), Box<dyn std::error::Error>> {
    let pinned = pin_thread();
    let hz = calibrate_hz();
    let overhead = median_overhead(8_192);
    let text = std::fs::read_to_string(input)?;
    let mut scratch = Scratch::new();
    let mut out = EncodedTxV1::boxed();
    let mut rows = Vec::new();
    let mut encode_failures = 0u64;

    for (line_no, line) in text.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }
        let raw = decode_hex(&json_str(line, "raw_hex"))
            .map_err(|err| format!("{}:{}: {err}", input.display(), line_no + 1))?;
        let status = encode_transaction_v1(&raw, &mut scratch, &mut out);
        if status != Status::Ok {
            encode_failures += 1;
            continue;
        }
        let bytes_copied = copied_input_bytes(&out);
        let max_ix_accounts = out.instructions[..out.header.instruction_count as usize]
            .iter()
            .map(|ix| ix.account_count)
            .max()
            .unwrap_or(0);
        warmup(&raw, &mut scratch, &mut out, 5_000);
        let ((ns, cycles), allocs) =
            counting_alloc::measure(|| batch_encode(&raw, &mut scratch, &mut out, batch_iters as u64));
        let mut sample_cycles = vec![0u64; samples as usize];
        let (_, sample_allocs) =
            counting_alloc::measure(|| sample_encode(&raw, &mut scratch, &mut out, &mut sample_cycles));
        let allocs_per_tx = (allocs as f64) / (batch_iters as f64);
        let sample_allocs_per = sample_allocs as f64 / samples as f64;
        let allocs_per_tx = allocs_per_tx.max(sample_allocs_per);
        let sample_ns = sample_cycles
            .into_iter()
            .map(|c| {
                let corrected = c.saturating_sub(overhead) as f64;
                corrected / hz * 1e9
            })
            .collect();
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
            sample_ns,
        });
    }

    let report = render(&rows, encode_failures, hz, overhead, pinned, samples, batch_iters, input);
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(output, report)?;
    eprintln!("bench wrote {}", output.display());
    Ok(())
}

fn render(
    rows: &[TxMeasure],
    failures: u64,
    hz: f64,
    overhead_cycles: u64,
    pinned: bool,
    samples: u32,
    batch_iters: u32,
    input: &Path,
) -> String {
    let mut md = String::new();
    md.push_str("# framed-tx-v1 encode benchmark\n\n");
    md.push_str(&format!("Schema `{FEATURE_SCHEMA}`. Input `{}`.\n\n", input.display()));
    md.push_str("## Method\n\n");
    md.push_str("- Release build, `opt-level=3`, thin LTO, `codegen-units=1`, `panic=unwind`.\n");
    md.push_str(&format!(
        "- Thread affinity pin: {}.\n",
        if pinned { "succeeded (mask bit 2)" } else { "not applied" }
    ));
    md.push_str(&format!(
        "- TSC frequency calibrated against `Instant` over ~100 ms: {:.3} GHz. Timer overhead median: {overhead_cycles} cycles.\n",
        hz / 1e9
    ));
    md.push_str(&format!(
        "- Per transaction: 5 000 warmup encodes, then a tight batch of {batch_iters} encodes timed with `Instant` and `rdtsc`, then {samples} single-shot `rdtsc` samples.\n"
    ));
    md.push_str("- Single-shot percentiles subtract the median empty `rdtsc` pair and convert with the calibrated frequency. The batch mean has no per-call timer pair inside the loop; use it as the cost estimate. Percentiles describe the single-shot distribution, which still includes residual measurement noise.\n");
    md.push_str("- Heap allocations are counted by a process-wide allocator armed only around the timed batch and the timed samples. Buffers are allocated before the counter is armed.\n");
    md.push_str("- Bytes copied are the instruction data heads written into `EncodedTxV1` (at most 32 bytes per instruction). Pubkeys are hashed in place.\n");
    md.push_str("- No figure below was selected or edited to approach 1 µs.\n\n");

    md.push_str("## Pilot maxima versus V1 capacities\n\n");
    md.push_str("| quantity | observed max | V1 capacity |\n|---|---:|---:|\n");
    let max_raw = rows.iter().map(|r| r.raw_len).max().unwrap_or(0);
    let max_ix = rows.iter().map(|r| r.instructions).max().unwrap_or(0);
    let max_acc = rows.iter().map(|r| r.accounts).max().unwrap_or(0);
    let max_look = rows.iter().map(|r| r.lookups).max().unwrap_or(0);
    let max_sig = rows.iter().map(|r| r.signatures).max().unwrap_or(0);
    let max_data = rows.iter().map(|r| r.max_ix_data).max().unwrap_or(0);
    let max_ix_acc = rows.iter().map(|r| r.max_ix_accounts).max().unwrap_or(0);
    md.push_str(&format!("| raw bytes | {max_raw} | packet payload 1232; encoder accepts larger until a table fills |\n"));
    md.push_str(&format!("| instructions | {max_ix} | {MAX_INSTRUCTIONS} |\n"));
    md.push_str(&format!("| expanded accounts | {max_acc} | {MAX_ACCOUNTS} |\n"));
    md.push_str(&format!("| ALT lookups | {max_look} | {MAX_LOOKUP_TABLES} |\n"));
    md.push_str(&format!("| signatures | {max_sig} | not stored; shortvec u16 and must match the header |\n"));
    md.push_str(&format!("| max instruction data bytes | {max_data} | shortvec u16 |\n"));
    md.push_str(&format!("| max accounts in one instruction | {max_ix_acc} | shortvec u16, indexes are u8 |\n\n"));
    md.push_str(&format!(
        "Successful encodes: {}. Encode failures skipped: {failures}.\n\n",
        rows.len()
    ));

    md.push_str("## Per transaction\n\n");
    md.push_str("| tx | bytes | version | ixs | accounts | alt | batch ns | batch cycles | single p50 ns | p99 ns | allocs/tx | bytes copied |\n");
    md.push_str("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for row in rows {
        let mut s = row.sample_ns.clone();
        s.sort_by(|a, b| a.partial_cmp(b).unwrap());
        md.push_str(&format!(
            "| {} | {} | {} | {} | {} | {} | {:.1} | {:.1} | {:.1} | {:.1} | {:.2} | {} |\n",
            row.tx_index,
            row.raw_len,
            if row.version == 255 { "legacy" } else { "v0" },
            row.instructions,
            row.accounts,
            row.uses_alt,
            row.batch_ns,
            row.batch_cycles,
            pct(&s, 0.50),
            pct(&s, 0.99),
            row.allocs_per_tx,
            row.bytes_copied
        ));
    }
    md.push('\n');

    md.push_str("## Buckets\n\n");
    md.push_str("Percentiles pool every single-shot sample in the bucket. Batch ns is the unweighted mean of each transaction's batch mean (each transaction used the same iteration count).\n\n");
    md.push_str("| bucket | txs | batch ns | p50 ns | p90 ns | p99 ns | p99.9 ns | tx/s | cycles/tx | allocs/tx | bytes copied |\n");
    md.push_str("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    emit_bucket(&mut md, "all", rows);
    emit_bucket(&mut md, "legacy", &filter(rows, |r| r.version == 255));
    emit_bucket(&mut md, "v0", &filter(rows, |r| r.version == 0));
    emit_bucket(&mut md, "no ALT", &filter(rows, |r| r.uses_alt == 0));
    emit_bucket(&mut md, "ALT", &filter(rows, |r| r.uses_alt == 1));
    emit_bucket(&mut md, "bytes 0-399", &filter(rows, |r| r.raw_len < 400));
    emit_bucket(&mut md, "bytes 400-799", &filter(rows, |r| (400..800).contains(&r.raw_len)));
    emit_bucket(&mut md, "bytes 800+", &filter(rows, |r| r.raw_len >= 800));
    emit_bucket(&mut md, "ix 1-3", &filter(rows, |r| (1..4).contains(&r.instructions)));
    emit_bucket(&mut md, "ix 4-8", &filter(rows, |r| (4..9).contains(&r.instructions)));
    emit_bucket(&mut md, "ix 9+", &filter(rows, |r| r.instructions >= 9));
    md.push('\n');

    if let Some(all) = rows.iter().map(|r| r.batch_ns).reduce(f64::min) {
        md.push_str("## Target\n\n");
        let mean = rows.iter().map(|r| r.batch_ns).sum::<f64>() / rows.len() as f64;
        md.push_str(&format!(
            "Aspirational target for common transactions: batch-loop mean under 1000 ns. Measured mean across this pilot is {mean:.1} ns. Fastest transaction batch mean is {all:.1} ns.\n"
        ));
        let alloc_max = rows.iter().map(|r| r.allocs_per_tx).fold(0.0f64, f64::max);
        md.push_str(&format!(
            "Maximum measured heap allocations per timed encode: {alloc_max:.3}.\n"
        ));
    }
    md
}

fn emit_bucket(md: &mut String, name: &str, rows: &[TxMeasure]) {
    if rows.is_empty() {
        md.push_str(&format!("| {name} | 0 | | | | | | | | | |\n"));
        return;
    }
    let mut pooled = Vec::new();
    for row in rows {
        pooled.extend_from_slice(&row.sample_ns);
    }
    pooled.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let batch = rows.iter().map(|r| r.batch_ns).sum::<f64>() / rows.len() as f64;
    let cycles = rows.iter().map(|r| r.batch_cycles).sum::<f64>() / rows.len() as f64;
    let allocs = rows.iter().map(|r| r.allocs_per_tx).sum::<f64>() / rows.len() as f64;
    let copied = rows.iter().map(|r| r.bytes_copied).sum::<u64>() as f64 / rows.len() as f64;
    let tps = if batch > 0.0 { 1e9 / batch } else { 0.0 };
    md.push_str(&format!(
        "| {name} | {} | {batch:.1} | {:.1} | {:.1} | {:.1} | {:.1} | {tps:.0} | {cycles:.1} | {allocs:.3} | {copied:.1} |\n",
        rows.len(),
        pct(&pooled, 0.50),
        pct(&pooled, 0.90),
        pct(&pooled, 0.99),
        pct(&pooled, 0.999),
    ));
}

fn filter(rows: &[TxMeasure], pred: impl Fn(&TxMeasure) -> bool) -> Vec<TxMeasure> {
    rows.iter()
        .filter(|r| pred(r))
        .cloned()
        .collect()
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
        std::hint::black_box(bench_harness::rdtsc());
    }
    let cycles = bench_harness::rdtsc().wrapping_sub(c0) as f64;
    let ns = start.elapsed().as_nanos() as f64;
    cycles / ns * 1e9
}

fn median_overhead(n: usize) -> u64 {
    let mut buf = vec![0u64; n];
    sample_overhead(&mut buf);
    buf.sort_unstable();
    buf[n / 2]
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
    let i = line.find(&pat).expect(key);
    let rest = &line[i + pat.len()..];
    let end = rest.find('"').unwrap();
    rest[..end].to_string()
}

fn json_u64(line: &str, key: &str) -> u64 {
    let pat = format!("\"{key}\": ");
    let i = line.find(&pat).expect(key);
    let rest = &line[i + pat.len()..];
    let end = rest.find([',', '}']).unwrap();
    rest[..end].trim().parse().unwrap()
}

fn decode_hex(s: &str) -> Result<Vec<u8>, String> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return Err("odd hex length".into());
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    for c in b.chunks(2) {
        out.push((hex(c[0])? << 4) | hex(c[1])?);
    }
    Ok(out)
}

fn hex(c: u8) -> Result<u8, String> {
    match c {
        b'0'..=b'9' => Ok(c - b'0'),
        b'a'..=b'f' => Ok(c - b'a' + 10),
        b'A'..=b'F' => Ok(c - b'A' + 10),
        _ => Err("bad hex".into()),
    }
}
