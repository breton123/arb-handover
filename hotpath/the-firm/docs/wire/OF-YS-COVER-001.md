# OF-YS-COVER-001

Why reconstructed OrbitFlare txs do not match Yellowstone pAMM
signatures in the same window. Not FAST_CUSTOM. Not AUTH import.

YS jsonl is the **pAMM-account** Geyser filter. It is not a full
slot ledger. Vote txs are excluded at subscribe.

## Run

```text
./build/of_ys_cover --cap OF.cap --ys ys.jsonl --probe 100 \
  --probes-out probes.jsonl --slots-out slots.jsonl
```

## Per overlapping slot

```text
ys        pAMM-touching YS txs in that slot
of        reconstructed OF txs
inter     same signature
ys_only   YS sig not reconstructed
of_only   OF sig not in the YS pAMM set
```

## 100-sig probe funnel

```text
overlapping YS pAMM
 → NO_OF_SLOT     slot never in OF shred headers
 → ABSENT_RAW     slot seen, signature bytes not in any FEEDCAP packet
 → RAW_NO_RECON   bytes present, not in stream_txs
 → RECON_BADSIG   framed but sig verify failed
 → RECON_OK       reconstructed + verified
```

`ABSENT_RAW` / `FEED_MISSING` means OrbitFlare coverage/selection.
`RAW_NO_RECON` is then split with the SHRED-STATE-001 prefix rule:

```text
FEED_MISSING    slot absent or signature bytes not in FEEDCAP
WRONG_BRANCH    same shred index, two Merkle roots (not merely many FEC sets)
PREFIX_GAP      no data shred index 0, or sig shred is past first hole
ENTRY_PARSE     below watermark, slot emitted no txs
TX_PARSE        below watermark, other txs emitted, this sig not framed
CLASSIFIER      framed+verified but LUT/class failed
OK              framed, verified, (slot, tx_index) matches YS
```

SHRED-STATE-001: 1,140/1,140 selected txs reconstructed against SHYFT when
the capture/alignment was correct. A decodable suffix without a contiguous
origin is still `PREFIX_GAP`. Do not merge conflicting roots by index.
