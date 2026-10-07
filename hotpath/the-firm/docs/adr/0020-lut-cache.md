# LUT-001: sparse cache plus getAccountInfo, not a bigger warm file

## Context

Live LUT misses matched the research hole: a thin warm file and no
learner. The old ALT plane used a nonexistent JSON-RPC method.
Manually enlarging `warm.jsonl` does not scale and does not encode
lifecycle.

## Decision

1. Keep a static warm cache.
2. Learn sparse `(table, index) → address` and full tables from
   `getAccountInfo` deserialization off-path.
3. Canonical resolve via `txview_resolve_at` (no descriptor unique).
4. Miss: fail closed, do not apply; DIRTY only static-key pools we
   already track.
5. Quarantine on address conflict. Do not trust provider pre-resolved
   address lists.

## Alternatives Considered

- Bigger warm.jsonl: rejected; research showed dynamic topology.
- `getAddressLookupTable` RPC: rejected; method does not exist.
- Blind provider loaded-keys: rejected; 9 finalized mismatches.

## Consequences

- Coverage should climb as the miss file is fetched, without applying
  unresolved txs.
- Full SlotHashes deactivation cooldown is still not certified.
