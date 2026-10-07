# PUMPSTATE-001: framed tx to Pump overlay

## Context

Reconstruction can emit raw versioned transactions. The old compact
Pump overlay already predicts poststate for sell / buy_exact_quote_in /
exact-out buy. Ingress decode mixed framing, Pump discs, and 64-key
limits. LUT loading used per-table append, not canonical v0 order.

## Decision

1. `txview_decode` is message-only. `txview_resolve` loads writable
   keys from every table, then readonly keys, fail closed on miss.
2. `txview_bind` + existing `overlay`/`apply_tx`/`pump_*` produce
   terminal state. One publish after the whole transaction.
3. Pool grade is EXACT or DIRTY. Unsupported Pump discs, LUT misses
   and apply failure dirty the pool. Bonding stays out.
4. Bootstrap CompactState from the existing trusted snapshot path.
   Shreds do not invent prestate.

## Alternatives Considered

- Generic SVM: rejected; first profit is the proven Pump projection.
- Wait for account notifications to apply: rejected; that is the lag
  we are removing after bootstrap.
- Publish after the first Pump ix: rejected; NON_OBVIOUS_EDGES
  revisits the same pool inside one tx.

## Consequences

- Exact live % is zero until AUTH/bootstrap is attached.
- LUT jsonl is coverage, not a lifecycle certificate.
