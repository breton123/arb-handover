# LIVE-TRUTH-001 synchronized OF + YS window

## Context

SLOT-STREAM-002 reconstructs verified txs. The five DIRECT_PUMP hits
were bonding, not pAMM. The remaining question is whether a real
PumpSwap tx can go OF packet → exact local publish faster than
Yellowstone state.

## Decision

A dedicated replay (`live_truth`) joins:

- OrbitFlare FEEDCAP1
- Yellowstone jsonl (`slot`, `index`, `sig_hex`, `version`, optional clocks)
- offline LUT jsonl
- existing CompactState AUTH (if seeded)

One stream per numeric slot is allowed **only** for this corpus
experiment. It is not SHRED_EXACT. Branch/root identity is a later
gate.

## Alternatives Considered

- RPC getBlock as truth: useful offline, not a YS substitute for the
  live race.
- Invent AUTH from invert-V: rejected.

## Consequences

Headline clocks: first OF packet → exact local publish vs first OF
packet → YS state. DLMM untouched. Bonding stays out of FAST_CUSTOM.
