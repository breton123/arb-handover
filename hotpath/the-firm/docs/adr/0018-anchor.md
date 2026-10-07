# ANCHOR-001: finalized per-pool context.slot

## Context

A processed `getMultipleAccounts` `context.slot` can name an **open**
bank. Skipping all OF txs with `slot <= that value` then drops later
same-slot transactions. GetSlot and account fetch can also race.
`minContextSlot` is a floor, not an exact historical bank.

## Decision

1. Buffer OF first; retain enough history to bridge finalization lag.
2. Snapshot with `commitment=finalized`. `anchor_slot` is that
   response's `context.slot` for the request that returned pool, base
   vault, quote vault, and global_config together.
3. Do not arm unless that vector is complete and decodes.
4. Catch-up: `tx.slot <= pool->anchor_slot` already reflected;
   `tx.slot > pool->anchor_slot` apply.
5. GetSlot is an optional live hint, never the boundary.
6. DIRTY recovery is the same finalized fetch, new per-pool anchor,
   replay `> S`.

## Alternatives Considered

- Processed/confirmed snapshot plus intra-slot RPC index: rejected;
  cold path can wait for a frozen bank.
- Pin RPC to a prior GetSlot via minContextSlot: rejected; not exact.
- One process-wide S: rejected; blocks late discovery.

## Consequences

- Startup and repair lag by finalization. OF must buffer across it.
- `slot <= S` skip is valid because S is a completed slot.
