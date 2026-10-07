# LIVE-TRUTH-001

Synchronized OrbitFlare + Yellowstone window. Not SHRED_EXACT.
Not DLMM. Bonding is not FAST_CUSTOM.

## Inputs

```text
OrbitFlare FEEDCAP1
Yellowstone jsonl   slot, index, sig_hex, version
                    optional t_ys_tx, t_ys_state
configs/luts/*.jsonl   offline warm
CompactState AUTH      if already seeded (caps alone have none)
```

## Run

Frankfurt (do not bind `:20001`; `feed_live` and `state008` already own OF/AUTH):

```text
tools/yellowstone/start_ys_pamm.sh /data/bsc/captures/live_truth/ys.jsonl
tools/yellowstone/follow_of_caps.sh
```

Replay:

```text
./build/live_truth --cap OF.cap --ys ys.jsonl --luts configs/luts/warm.jsonl --cands pamm.jsonl
./build/wire_to_state003 --cap OF.cap --dump-txs recon.jsonl --luts …
python tools/yellowstone/compare.py --recon recon.jsonl --truth ys.jsonl
```

## Scoreboard

```text
verified_txs
pamm_swaps
ys_sig_hit / ys_slot_ok / ys_index_ok
certified_prestate
FAST_CUSTOM_exact
NO_LUT / NO_PRESTATE / TX_MISMATCH
FAST_CUSTOM_FALLBACK / EXACT_MATCH / EXACT_MISMATCH
```

Each pAMM buy/sell is exactly one of those six outcomes. EXACT_MATCH
means local FAST_CUSTOM published and Yellowstone did not contradict
(slot/index identity, and post_eq when present). EXACT_MISMATCH is
only used when a YS poststate compare is present and fails.

## Clocks (when a pAMM swap admits)

```text
t_first_packet
t_prefix_ready      (contiguous watermark)
t_tx_complete
t_prestate_ready
t_fast_custom_start
t_publish
t_yellowstone_tx
t_yellowstone_state
```

Headline: OF first packet → exact local publish vs OF first packet → YS state.

## Not this experiment

- One stream per numeric slot is allowed here only.
- Branch/root identity lands **after** a real pAMM exact match.
- Do not invent AUTH. Do not RPC on the reconstruct path.
