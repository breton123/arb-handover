# CORE-009 — six-venue universe vs the 86 GB shred corpus

Locked `classify.c` (2-ID AVX2) and `route0` were not mutated.
The raw `FEEDCAP1` files were not written (`chmod 444` still on disk).

Replay path (pass 2, clean timer):

```
86 GB raw .cap
  → shred parse
  → classify_n (2..6 IDs, every shred)
  → decode
  → pool_idx
  → apply_swap
  → routes_by_pool[pool_idx]
  → quote + size
  → opportunity_t
  → nonce + template[family] + sign
  → leader_send(STUB)
```

`tsc_hz = 4194228833`. 74,688,597 shreds. 625,580 framed.

## Classifier (all 74.7M shreds)

| IDs | mean cyc | p50 | p99 |
|-----|----------|-----|-----|
| 2 | 327 | 340 | 460 |
| 3 | 414 | 420 | 548 |
| 4 | 500 | 508 | 716 |
| 5 | 594 | 588 | 796 |
| 6 | 644 | 628 | 884 |

2 → 6 IDs is **327 → 644 cyc**. Linear-ish. Not 3000. No generated discriminator.

Relevant shreds: 5.34M (2 IDs) … 6.91M (6 IDs).

## Framed by protocol

| proto | framed |
|-------|--------|
| Pump | 296,468 |
| DAMM v2 | 276,546 |
| DLMM | 22,473 |
| CLMM | 10,799 |
| CPMM | 10,371 |
| Orca | 8,923 |

DAMM is not a rounding error in this corpus.

## `T_actionable → signed` (6-ID universe only)

| | n | p50 | p99 | p999 |
|--|---|-----|-----|------|
| decode + apply + eval | 625,580 | 11.9 µs | 15.2 µs | 16.9 µs |
| … + nonce + patch + sign + stub | 625,580 | **26.7 µs** | 30.8 µs | 32.8 µs |

Sign is still the ~15 µs Ed25519 term. Local work is ~12 µs. Still ~20 µs-class (sign-dominated). Do not stop expansion.

Pass 1 reported 43.6 µs because the timer included five universe evals. That number is discarded.

## Universe size vs eval (seeded 8-token graph)

| Universe | Pools | Routes | eval p50 | p99 | p999 |
|----------|-------|--------|----------|-----|------|
| DLMM+Pump | 23 | 72 | 3.0 µs | 3.2 µs | 6.5 µs |
| +CLMM | 31 | 174 | 4.7 µs | 5.5 µs | 8.6 µs |
| +CPMM | 39 | 320 | 6.3 µs | 7.3 µs | 10.4 µs |
| +DAMM | 47 | 510 | 7.8 µs | 8.9 µs | 12.0 µs |
| +Orca | 55 | 744 | 9.4 µs | 12.1 µs | 13.9 µs |

Eval grows with **routes_by_pool[changed]** fanout on a shared 8-mint toy graph, not with global route count.

Dummy-route proof (`core009` on-box):

| Global routes | hot_eval_pool[0] p50 |
|---------------|----------------------|
| 10,002 | 430 ns |
| 100,002 | 430 ns |
| 1,000,002 | 340 ns |

`routes_by_pool[]` does not walk the rest of the universe. 10k / 100k / 1M stay in the same few-hundred-ns bin.

V1 compiler: 2-hop + 3-hop only. Seed-6: 240 two-hop + 504 three-hop = 744.

## What is live vs placeholder

Live tonight: adapters (`apply` / `quote_exact_in`), `classify_n`, `find_prog_id_n`, dense `pool_idx` metadata, 2/3-hop compiler, hot walk, route families 1–4 as extra templates.

Not live: historical CLMM/Orca tick arrays (fail-closed, same as missing DLMM bins), real family-1..4 account vectors (patch offsets only), economic quotes on this replay (synthetic S; apply of 0.01 SOL creates the measured arb).

route0 layout is frozen.
