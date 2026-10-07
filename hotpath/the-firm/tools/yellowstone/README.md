# Yellowstone / truth compare

SLOT-STREAM-002 freeze gate: match reconstructed signatures to an
external truth file.

```text
./build/wire_to_state003 --cap FILE.cap --dump-txs recon.jsonl
python tools/yellowstone/compare.py --recon recon.jsonl --truth truth.jsonl
```

Truth may be Yellowstone-exported jsonl or Frankfurt
`/home/louis/arb-cap/oneshot/entry_truth.jsonl` (`sig_hex`, `slot`).

A passing freeze is: sampled slots, same signature, same slot, and
(when present) matching entry/tx ordinals.

Live YS capture (Frankfurt, tokens from `~/.arb-*.env` only):

```text
tools/yellowstone/start_ys_pamm.sh /data/bsc/captures/live_truth/ys.jsonl
```

Writes `ys_tx` / `ys_state` jsonl for `live_truth --ys`. Does not
print tokens. Does not replace CompactState AUTH.
