# PUMP-ORACLE-001 differential admission

## Context

The transition core is sub-µs. We do not yet have proof that certified
prestate plus our apply matches a reference bank on live Pump
transactions. The old soak (state008) has geyser `published_s` and
explained residuals (missing virtual, wrong association).

## Decision

1. Freeze a text corpus of captured/certified cases.
2. Judge each case: FAST_CUSTOM_EXACT / FALLBACK_REQUIRED / UNKNOWN /
   UNEXPLAINED.
3. Compare pool, token, WSOL native, system, incarnation, cert, overlay.
4. Incomplete prestate cannot become identity.
5. Block DLMM until UNEXPLAINED is zero on the held-out set.

## Alternatives Considered

- Treat engine-generated poststate as the bank: rejected; circular.
- Soften exact-out invert failures for coverage: rejected.
- Run Agave in-process: out of scope; fixtures carry bank poststate.

## Consequences

- `test_pump_oracle` is the Pump admission gate.
- Expanding the corpus is an import problem, not a kernel change.
