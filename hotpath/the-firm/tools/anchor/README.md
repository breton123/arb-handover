# Pump snapshot adapter (cold start)

Buffer OrbitFlare **before** `snapshot.py`. Each jsonl row's `slot` is
that **finalized** fetch's `context.slot`. GetSlot is a live hint only.
The OF buffer must cover finalization lag. See `docs/wire/ANCHOR-001.md`.
