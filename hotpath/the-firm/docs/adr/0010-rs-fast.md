# RS-FAST-001 cached recovery matrices

## Context

WIRE-TO-STATE-003 recovered 2771/2771 authenticated sets, but mean RS
time was ~2.2 ms/set because each recover inverted a Vandermonde matrix
and evaluated a polynomial per column.

## Decision

For Solana's fixed 32 data + 32 coding layout, cache the recovery
matrix R[missing][present] keyed by the 64-bit have-mask. Apply R with
table GF(256) axpy (AVX2 nibble shuffle when built with AVX2). Do not
invert per column or per set on a cache hit.

## Alternatives Considered

- Import Firedancer SIMD RS: faster, large dependency. Deferred.
- Cauchy RS: not the Solana Vandermonde evaluation points.

## Consequences

First occurrence of an erasure pattern pays invert cost. Recurring
patterns (especially 1–4 missing data shreds) hit cache. Benchmark by
missing-shard count, not only average.
