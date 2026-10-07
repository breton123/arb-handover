# firm-papertrade

Paper-trade the current arb model on the live feed and measure the hot path, end to end, without sending.

This repository is the hand-over package for that work. It is a snapshot of three things that today live in
different places, plus the documentation and targets to finish the job:

| Directory | What it is | Origin |
|---|---|---|
| `offline-ml/` | Research pipeline: framed-tx encoder (Rust), `ml-prep` (Rust), Python package `firm_ml`, weekly fleet scripts, notebooks, experiments, model export | `the-firm-offline` (local) |
| `hotpath/the-firm/` | C data plane: OrbitFlare / DoubleZero / Blockspace feed race, shred race and assembly, TXFRAME, ALT cache, state engine experiments | `~/the-firm` on the Frankfurt box |
| `hotpath/arb-exec/` | Opportunity → signed transaction: templates, Ed25519 sign, durable nonce ring, leader send, SWQoS sender, on-chain executor program | `~/arb-exec` |
| `hotpath/arb-core/` | Earlier frozen opportunity core (DLMM ↔ PumpSwap) and `opportunity_t` boundary | `~/arb-core` |
| `hotpath/arb-feed/` | Earlier feed layer, including the Rust OrbitFlare client (`flowra`) | `~/arb-feed` |
| `hotpath/arb-exec-live/`, `hotpath/arb-cap/` | Live executor program variants; earlier paper and live trial scripts (`paper004`, `live001/2`) | box |
| `docs/` | System overview, model card, feature contract, decision policy, paper-trading spec, latency plan, data and infra, results | written for this hand-over |

Start with **[HANDOVER.md](HANDOVER.md)**: scope, milestones, targets and acceptance criteria.

## Reading order

1. [HANDOVER.md](HANDOVER.md) — what to build and how it will be judged
2. [docs/01-system-overview.md](docs/01-system-overview.md) — the pipeline from packet to send, and where each piece lives
3. [docs/02-model-card.md](docs/02-model-card.md) — the model being paper-traded: data, labels, protocol, metrics, limits
4. [docs/03-feature-contract.md](docs/03-feature-contract.md) — every model input and exactly how to compute it from a raw transaction
5. [docs/04-decision-policy.md](docs/04-decision-policy.md) — candidates, packs, expected value, thresholds per send budget
6. [docs/05-paper-trading-spec.md](docs/05-paper-trading-spec.md) — what the paper trader does, logs, and how it is scored
7. [docs/06-latency-eval.md](docs/06-latency-eval.md) — latency budget, measurement method, targets
8. [docs/07-data-and-infra.md](docs/07-data-and-infra.md) — Tigris paths, the Frankfurt box, the model bundle, credentials
9. [docs/08-results-so-far.md](docs/08-results-so-far.md) — what the offline work found
10. [docs/09-leakage-audit.md](docs/09-leakage-audit.md) — how far to trust those results

## Boundaries

- **The labeller is out of scope.** It is being rebuilt in parallel (more venues, VM quoting, realised-arb labels,
  candidate packs as its last stage). This repo consumes its outputs through the model bundle and published
  labels; it never changes the labeller or the collector.
- **No live sends.** Paper trading builds and signs nothing that reaches the network unless a live canary is
  explicitly approved later (milestone M6).
- **Secrets are not in this repo.** Wallets, keypairs, `.env` files and API tokens were excluded from the
  snapshot (see [hotpath/SNAPSHOT.md](hotpath/SNAPSHOT.md)). Get credentials from the owner; never commit them.
