# PUMP-ORACLE-002 corpus-scale admission

## Context

PUMP-ORACLE-001 proved admission semantics on 7 held-out cases. The
state008 soak still has 4,118 Pump mismatches. DLMM must not start
until those cases classify without `UNEXPLAINED`.

## Decision

1. Import mismatch JSON + optional `n015_tx` into jsonl.
2. Rebuild certified pre → `OrderedTx` → custom engine → post.
3. Classify only FAST_CUSTOM_EXACT / FALLBACK_REQUIRED / UNKNOWN /
   UNEXPLAINED.
4. Never inject inverted virtual quote as certified pre.
5. Gate: 0 UNEXPLAINED. Coverage may be fallback/unknown.

## Alternatives Considered

- Require 100% FAST_CUSTOM: rejected; missing AUTH `V` is fallback.
- Treat invert(`V`) as certified: rejected; circular vs the bank.
- Start DLMM in parallel: rejected; Pump admission is unproven at
  corpus scale until the soak tree is mounted and unexplained is 0.

## Consequences

- `test_pump_oracle002` is the scale gate.
- Frozen jsonl: 4118 usable Pump rows, 0 UNEXPLAINED, 0
  FAST_CUSTOM-but-wrong (2026-09-26). AUTH never carried `V`, so
  FAST_CUSTOM coverage is 0% and that is a correct fallback, not a
  kernel miss.
- WIRE-TO-STATE-001 is unblocked. DLMM is not.
