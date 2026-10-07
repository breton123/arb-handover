# WIRE-CLASSIFY-001

Classification of reconstructed transactions. Not Pump apply. Not DLMM.

## Tags (exclusive, first match)

```text
DIRECT_PUMP
DIRECT_DLMM
KNOWN_ROUTER
WATCHED_ACCOUNT_TOUCH
UNRESOLVED_ALT
OTHER
```

Watched keys today: Pump AMM, Pump bonding, Meteora DLMM. Router: Jupiter v6.

## Yellowstone freeze

```text
./build/wire_to_state003 --cap FILE --dump-txs recon.jsonl
python tools/yellowstone/compare.py --recon recon.jsonl --truth TRUTH.jsonl
```

Frankfurt `entry_truth.jsonl` (400 sigs) does **not** overlap the
2026-09-26 OrbitFlare cap (`compared=0`). Need a Yellowstone dump from
the same slots before freezing SLOT-STREAM-002.

The five DIRECT_PUMP hits are bonding-curve, not PumpSwap:
`docs/wire/PUMP-FIVE.md`.
