# SLOT-STREAM-002 contiguous slot byte stream

## Context

ENTRY-STREAM-001 raised entries to 7 192 and stream_txs to 309, all
legacy, v0/v1 = 0. Isolated DATA_COMPLETE islands (~617 KiB) were
parsed as standalone buffers. Authenticated shreds were much larger.
That is false framing, not missing RS.

## Decision

One incremental `Vec<Entry>` parser per slot. Insert data shreds by
index. Append only when `index == watermark` (origin 0). Carry leftover
bytes across DATA_COMPLETE. A gap blocks promotion. No `[1,512]`
heuristic on the live path. Fee-payer Ed25519 via OpenSSL when linked.

## Alternatives Considered

- Per-complete parse reset: rejected; txs and vecs continue across
  ranges.
- Wait for SLOT_COMPLETE: unnecessary latency.

## Consequences

Yellowstone slot agreement is not implemented here. Branch/root
identity is still first-writer per slot index. Pump/AUTH/DLMM
untouched.
