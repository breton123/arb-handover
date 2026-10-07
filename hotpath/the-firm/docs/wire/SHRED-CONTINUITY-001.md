# SHRED-CONTINUITY-001

## Context

Pool `97e81bb1…` proved CompactState can stay EXACT while the chain
moves: snapshot at 450882490 matched vault prestate, then 1068
successful vault transactions never appeared in the live_state walk of
the freeze OrbitFlare capture. LIVE-STATE apply/Pump math is paused.

A validator does not require every original data shred on the wire.
Turbine can be lossy; FEC (32 data + 32 coding) and Repair fill holes.
Our live path is:

```text
OrbitFlare → received packets → first-writer race
         → data shreds only into a contiguous index-0 prefix
         → frame DATA_COMPLETE batches
```

`src/shred/fec.c` exists. `live_state_001` does not call it
(`shred_ingest_packet` → `shred_prefix_push` data only). A hole at
index 3 stops the prefix; later data shreds in the same slot are not
concatenated.

## Question

Can this capture reconstruct the same ordered transaction prefix as
finalized Solana for slots we would claim complete?

Per missing successful vault tx in `(450882490, 450883470]`:

| Class | Meaning |
|---|---|
| A RAW_PRESENT | Signature sits in the **data-only** contiguous prefix. Reconstruction/framing missed it. |
| B FEC_RECOVERABLE | Signature sits in the prefix only after `shred_fec_push` recovered data shreds. Lack of FEC on the live path is the cause. |
| C FEED_MISSING | Not in the FEC-extended prefix. Capture lacks enough data+coding (or the tx is an orphan past an unrecoverable hole). |

`raw_orphan` counts C probes whose 64-byte signature still appears in
some received data shred past the unfilled hole.

## Command

```text
python3 tools/pump/walk_sigs.py run/ab1/walk97.jsonl run/ab1/missing1068.jsonl
./build/shred_continuity --cap FIRMCAP1 --probes run/ab1/missing1068.jsonl \
    > run/ab1/continuity_slots.jsonl
```

Per-slot JSON on stdout: data/code counts, first hole with and without
FEC, concat lengths. Summary on stderr.

Only COMPLETE or successfully RECOVERED history should advance EXACT
state. Do not treat “framed OF tx after snapshot” as a ledger prefix.
