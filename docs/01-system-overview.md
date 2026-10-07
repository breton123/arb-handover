# System overview

## The live path

```text
OrbitFlare UDP (feed 1)      DoubleZero (2)      Blockspace (3)
          │                        │                   │
          └────────── net_feed_t, recvmmsg into per-feed SPSC rings ──────────┘
                                   │  net_packet_t {source_id, rx_ns, rx_tsc, len, data}
                                   ▼
                     net_race_t: merge by first arrival
                                   ▼
          shred envelope → shred identity → first-writer-wins race (WIN copies only)
                                   ▼
          slot prefix assembly → DATA_COMPLETE batches → Vec<Entry>
                                   ▼
          TXFRAME: legacy / v0 / v1 (0x81) wire frame of each transaction
                                   ▼
          ALT resolve (LUT cache: static warm + sparse (table,index) + off-path fetch)
                                   ▼
   ┌──────────────────────────── NEW (this hand-over) ─────────────────────────────┐
   │ feature builder → pool lookup (touched pools → candidate routes)              │
   │ → arb classifier P(arb) × size model E[profit] = EV → threshold → top-L packs │
   └───────────────────────────────────────────────────────────────────────────────┘
                                   ▼
          arb-exec: template memcpy + patch (amount, direction, min_profit, nonce, CU price)
                   → durable nonce claim → Ed25519 sign → leader_send / SWQoS
```

Everything above the box is built and measured in `hotpath/the-firm`. Everything below it is built in
`hotpath/arb-exec`. The box in the middle is the work in this hand-over.

## Where each piece lives

| Stage | Code | Docs | Status |
|---|---|---|---|
| Feed receive, rings, capture `FIRMCAP1` | `the-firm/src/net`, `include/net` (`feed.h`, `race.h`, `ring.h`, `capture.h`) | `the-firm/src/net/README.md`, `docs/adr/0001-ingest-feed-race.md` | Built; OrbitFlare is feed 1 |
| Shred race (first writer wins), FEC, assembly | `the-firm/src/shred` | `src/shred/README.md`, `docs/adr/0002-shred-keyed-race.md`, `docs/wire/SHRED-CONTINUITY-001.md` | Built and benchmarked (`docs/benchmarks/shred.md`) |
| Entry / slot stream | `the-firm/src/wire` | `docs/adr/0011-entry-stream.md`, `0012-slot-stream.md` | Built |
| Transaction framing (legacy, v0, v1) | `the-firm/src/ingress`, `src/tx/view.c` | `docs/wire/TXFRAME-001.md` | Built; gold parity on the frozen OrbitFlare capture |
| ALT resolution | `the-firm/src/tx/resolve.c`, `tools/lut` | `docs/wire/LUT-001.md`, `docs/adr/0020-lut-cache.md` | Built; 89.8% sparse resolve rate measured; miss = do not apply |
| Wire classification | `the-firm/src/wire` | `docs/wire/WIRE-CLASSIFY-001.md` | Built |
| Pool state engine (Pump, shadow) | `the-firm/src/state`, `src/transitions`, `apps/live_state` | `docs/adr/0003-state-engine.md`, `0019-live-state.md` | Experimental; **not used by this model** |
| Feature builder | — | [03-feature-contract.md](03-feature-contract.md) | **To build (M1)** |
| Model inference | — | [02-model-card.md](02-model-card.md) | **To build (M2)** |
| Decision policy and packs | — | [04-decision-policy.md](04-decision-policy.md) | **To build (M3)** |
| Transaction template, patch | `arb-exec/src/tx_template.c`, `route0*.c`, `route_fam.c` | `arb-exec/README.md` (EXEC-001, 002) | Built for specific route families |
| Signing | `arb-exec/src/sign.c` (libsodium) | EXEC-003 | Built; Ed25519 ≈ 15 µs p50 |
| Durable nonce ring | `arb-exec/src/nonce.c` | EXEC-004 | Built; claim ≈ 20 ns |
| Send | `arb-exec/src/leader.c`, `swqos/` (Rust) | EXEC-005 | Local UDP stub plus SWQoS sender |
| On-chain executor | `arb-exec/program`, `program_hops`, `program_dlmm2`, `arb-exec-live/` | `arb-exec/README.md` | Route families: DLMM ↔ PumpSwap, hops variants |

Older trees kept for reference: `arb-feed` (earlier feed layer, Rust OrbitFlare client `flowra`), `arb-core`
(frozen DLMM ↔ PumpSwap opportunity core, `opportunity_t`), `arb-cap` (paper and live trial scripts, including
`paper004` which validated 34 contemporaneous decisions against RPC).

## Why the model fits a stateless path

The model reads only the raw transaction (after framing and ALT resolution) and small tables computed offline from
labels (pool statistics, pool → route lists). It never needs live pool reserves or prices. Sizing a trade is left
to the on-chain executor, which reads pool state at execution time. That keeps the decision path free of state
management and inside a few microseconds.

## The gap to close for execution

`arb-exec` today expresses fixed route families (DLMM ↔ PumpSwap and hop variants). The model proposes routes over
the three labelled venues (Raydium CLMM, Orca Whirlpool, Meteora DLMM) and, once the labeller is rebuilt, more.
M5 measures how many decisions the executor can express, per route family. That report decides what executor work
comes next.
