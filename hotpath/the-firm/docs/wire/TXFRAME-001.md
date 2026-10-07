# TXFRAME-001

New reconstruction layer. Stops at a raw framed transaction.
Does **not** port `shred_slot_stream_parse`. That file is the
problem specimen.

```text
net_packet_t
 → shred envelope
 → shred identity / first-writer claim
 → WIN only: slot prefix from index 0
 → DATA_COMPLETE Vec<Entry> batches
 → versioned/legacy/v1 wire frame
```

No ALT, no Pump, no state. One bad tx is `STREAM_DESYNC` for the
rest of that entry vector. It does not freeze the slot assembler.

Layouts follow TriggerResearch `analyze_shred_state.transaction`
and arb-core `frame_v1_layout`: `0x81` is v1, not a shortvec.

## Fixture

Immutable capture (do not rotate):

```text
/home/louis/fixtures/orbitflare-20260927-002844.cap
```

Probes: `tests/fixtures/of_ys_cover/probes.jsonl` (100 YS pAMM sigs).

Expected cover classes (old stream): 18 FEED_MISSING, 24 PREFIX_GAP,
58 TX_PARSE. Target: `txframe_cover` frames those 58 and prints a
per-probe result (`OK` / `NEED_MORE_BYTES` / `BAD_*` /
`UNSUPPORTED_VERSION` / `STREAM_DESYNC` / `PREFIX_GAP`).
`PREFIX_GAP` stays incomplete. Prefix tables hold 2048 data shreds
so a 896-shred hole is not a table-capacity artefact.

Each `DATA_COMPLETE` shred (`flags & 0x40`) closes one `Vec<Entry>`
batch, matching TriggerResearch `analyze_shred_state` dataset
splits. The slot is not one outer vector.

Measured on the frozen 002844 cap: `gold_tx_parse=58 gold_ok=58`.
The 24 PREFIX_GAP / 18 FEED_MISSING rows stay outside that gate.

```text
./build/test_txframe
./build/txframe_cover --cap /home/louis/fixtures/orbitflare-20260927-002844.cap \
  --probes tests/fixtures/of_ys_cover/probes.jsonl
```
