# WIRE-TO-STATE-003

Host: Frankfurt `195.242.152.178`, gcc `-O3 -march=native`.
`/data/bsc/captures/orbitflare/orbitflare-20260926-224724.cap`

2026-09-27 after SLOT-STREAM-002.

## Scoreboard

| | ENTRY-STREAM-001 | SLOT-STREAM-002 |
| --- | ---: | ---: |
| packets | 1 732 555 | 1 732 555 |
| fec recovered / auth_ok | 2 771 / 2 771 | same |
| auth_shreds | 864 587 | 864 587 |
| slot_stream_bytes | 617 115 (islands) | **119 087 296** |
| watermark_advances | — | 124 066 |
| leftover_bytes (end) | — | 688 004 |
| entries / incremental | 7 192 | **26 496** |
| stream_txs | 309 | **22 796** |
| legacy / v0 / v1 | 309 / 0 / 0 | **22 419 / 377 / 0** |
| sig_ok / sig_fail | — | **22 621 / 175** |
| frozen / gap_blocked | — | 221 / 835 751 |
| pump / FAST_CUSTOM | 0 / 0 | 0 / 0 |
| ALT | 0 | 141 |

`v0 > 0` and `sig_ok ≈ stream_txs` means these are real frames, not the all-legacy false parse.

`v1 = 0` on this cap: no version-1 messages observed (Agave still mostly v0).

`gap_blocked` counts every insert with `index > watermark` (normal reordering), not unique holes.

Capture-TSC first_rx histograms on this run are **not trusted**: some batches had `tsc_first == 0` after take, so deltas explode. Fix before quoting p50/p99.

## WIRE-CLASSIFY-001 (same cap, 2026-09-27)

| | |
| --- | ---: |
| classify verified | 22 624 |
| DIRECT_PUMP | **5** |
| DIRECT_DLMM | 1 |
| KNOWN_ROUTER | 0 |
| WATCHED | 2 |
| UNRESOLVED_ALT | 1 |
| OTHER | 22 615 |
| FAST_CUSTOM exact | 0 |

Those 5 Pump programs are not decoded as buy/sell on any ix (bonding / other disc / not `ix[0]`-only anymore). Exact publish still needs AUTH.

## Yellowstone

`entry_truth.jsonl` (400 sigs, slots ~450389860) vs this cap: **compared=0**. Different window. SLOT-STREAM-002 is **not** frozen.

Clocks: `tsc_origin` is no longer cleared; p50 0 is same-TSC first/last shred, not the old wraparound. Branch/root streams still pending.
