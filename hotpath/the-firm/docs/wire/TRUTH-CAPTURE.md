# Same-slot OrbitFlare + Yellowstone truth

SLOT-STREAM-002 stays research-grade until this passes.

## What to capture (50–100 slots, same window)

1. OrbitFlare shreds → `rx_sink` FEEDCAP1 (existing).
2. Yellowstone (or equivalent committed txs) → jsonl:

```text
{"slot":N,"index":I,"sig_hex":"128 hex","version":0|1}
```

`index` is the slot transaction ordinal from the bank, not shred index.

## Compare

```text
./build/live_truth --cap OF.cap --ys ys.jsonl --luts configs/luts/warm.jsonl
./build/wire_to_state003 --cap OF.cap --dump-txs recon.jsonl --luts …
python tools/yellowstone/compare.py --recon recon.jsonl --truth ys.jsonl
```

See `docs/wire/LIVE-TRUTH-001.md`.

Pass: sampled slots, same `sig_hex`, same `slot`, and matching `index` when present.

## Do not

- Freeze the stream on `entry_truth.jsonl` (different slots).
- Hit RPC on the reconstruct hot path.
- Treat one-stream-per-numeric-slot as SHRED_EXACT.

## LUT warm (out of band)

Preload common address tables into `configs/luts/` before the window.
See `configs/luts/README.md`. Resolve locally at classify time.
