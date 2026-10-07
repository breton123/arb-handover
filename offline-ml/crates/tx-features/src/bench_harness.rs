//! Timing helpers for the benchmark binary.
//!
//! Not called by [`crate::encode_transaction_v1`]. The functions below do not
//! allocate; the caller owns every buffer.

use crate::features::encode_transaction_v1;
use crate::view::{EncodedTxV1, Scratch};
use std::hint::black_box;
use std::time::Instant;

#[inline]
pub fn rdtsc() -> u64 {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        core::arch::x86_64::_mm_lfence();
        core::arch::x86_64::_rdtsc()
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        0
    }
}

pub fn has_tsc() -> bool {
    cfg!(target_arch = "x86_64")
}

/// Back-to-back `rdtsc` deltas, used to subtract timer overhead from single-shot samples.
pub fn sample_overhead(dst: &mut [u64]) {
    for slot in dst.iter_mut() {
        let c0 = rdtsc();
        let c1 = rdtsc();
        *slot = c1.wrapping_sub(c0);
    }
}

/// One timed encode per `dst` slot. Writes raw cycle deltas, including timer overhead.
pub fn sample_encode(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV1, dst: &mut [u64]) {
    for slot in dst.iter_mut() {
        let c0 = rdtsc();
        let status = encode_transaction_v1(black_box(raw), black_box(scratch), black_box(out));
        let c1 = rdtsc();
        black_box(status);
        *slot = c1.wrapping_sub(c0);
    }
}

/// Tight loop. Returns `(elapsed_ns, elapsed_cycles)`.
pub fn batch_encode(
    raw: &[u8],
    scratch: &mut Scratch,
    out: &mut EncodedTxV1,
    iters: u64,
) -> (u128, u64) {
    let start = Instant::now();
    let c0 = rdtsc();
    let mut sink = 0i32;
    for _ in 0..iters {
        let status = encode_transaction_v1(black_box(raw), black_box(scratch), black_box(out));
        sink ^= status as i32;
    }
    let cycles = rdtsc().wrapping_sub(c0);
    let ns = start.elapsed().as_nanos();
    black_box(sink);
    (ns, cycles)
}

pub fn warmup(raw: &[u8], scratch: &mut Scratch, out: &mut EncodedTxV1, iters: u64) {
    let mut sink = 0i32;
    for _ in 0..iters {
        sink ^= encode_transaction_v1(raw, scratch, out) as i32;
    }
    black_box(sink);
}
