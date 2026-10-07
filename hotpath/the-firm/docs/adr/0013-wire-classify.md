# WIRE-CLASSIFY-001

## Context

SLOT-STREAM-002 produces verified transactions. Zero Pump on the
OrbitFlare cap is a classification question, not a reconstruction
question. Outer-instruction Pump detection misses CPI and LUT-loaded
program IDs.

## Decision

Tag every reconstructed tx as exactly one of: DIRECT_PUMP,
DIRECT_DLMM, KNOWN_ROUTER, WATCHED_ACCOUNT_TOUCH, UNRESOLVED_ALT,
OTHER. Resolve v0 LUT indexes when the table is in the LUT cache.
Missing LUT → UNRESOLVED_ALT. FAST_CUSTOM is only offered DIRECT_PUMP
with a decoded Pump buy/sell.

Yellowstone compare is an offline jsonl signature match
(`tools/yellowstone/compare.py`). No gRPC client in this change.

## Alternatives Considered

- Require an outer Pump ix before relevance: rejected.
- RPC LUT fetch in the hot path: rejected; cache only.

## Consequences

Pump/AUTH/DLMM apply unchanged. Branch/root streams still pending.
LUT cache starts empty, so live v0 with lookups count as
UNRESOLVED_ALT until tables are loaded.
