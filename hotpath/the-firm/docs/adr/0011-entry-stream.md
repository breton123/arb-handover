# ENTRY-STREAM-001 Vec&lt;Entry&gt; across FEC sets

## Context

Authenticated FEC recovery still produced almost no framed transactions.
A DATA_COMPLETE range is a bincode `Vec<Entry>`, not an independent
transaction container. Treating each completed set as starting at an
Entry (or a raw tx) misaligns by the leading `u64` count.

## Decision

Append authenticated data shreds in index order. On DATA_COMPLETE, parse
the concatenated bytes as `Vec<Entry>` when the leading count is in
`[1, 512]`. Fall back to the previous flat Entry / raw-tx walk for
fixtures that omit the vector prefix. Emit a tx only after
`ingress_decode_tx` returns a complete frame.

## Alternatives Considered

- Per-FEC-set tx parse: rejected; it is the current failure mode.
- Wait for SLOT_COMPLETE: unnecessary; each DATA_COMPLETE batch is a
  complete `Vec<Entry>` from the shredder.

## Consequences

Scoreboard adds authenticated shreds, contiguous bytes, entries, txs,
legacy/v0/v1, Pump. Pump/AUTH/DLMM apply paths are unchanged.
