# WIRE-TO-STATE-002

Real OrbitFlare `.cap` through the same stages as WIRE-TO-STATE-001.
Not a validator. Not DLMM. No Reed-Solomon recover.

## Path

```text
packet_rx
 → FEC / data-set completeness
 → authenticated framed tx (DATA_COMPLETE concat)
 → OrderedTx
 → key resolve
 → deps
 → certified prestate
 → apply
 → publish
```

Clocks kept separately:

| Clock | Meaning |
| --- | --- |
| `tx_ready_time` | capture TSC of the shred that completed the batch |
| `prestate_ready_time` | when AUTH was certified (`UINT64_MAX` = never) |
| `publish_time` | `rdtscp` after exact publish |

`tx_before_pre` is reconstruction-bound vs state-cert-bound.

## Buckets

| Bucket | Meaning |
| --- | --- |
| one-packet complete | single DATA_COMPLETE shred, framed tx extracted |
| multi-shred complete without FEC | contiguous data shreds, last DATA_COMPLETE |
| FEC-recovered | always 0 until RS exists (`fec-possible` counts coding-enough sets) |
| incomplete | data shreds still waiting for a contiguous complete run |
| ALT fallback | v0 address-table lookups |
| prestate fallback | missing V / fees / vaults / mints |
| FAST_CUSTOM exact | AUTH_READY + apply + publish |

Caps are shreds only. FAST_CUSTOM stays 0 unless CompactState already holds
certified AUTH. Do not invent invert-V as certified pre.

## Measure

```text
./build/wire_to_state002 --self-test
./build/wire_to_state002 --cap /data/bsc/captures/orbitflare/FILE.cap
```

Headline (capture TSC, not replay CPU):

```text
packet_rx → tx_complete
packet_rx → exact publish
tx_complete → exact publish
```

p50 / p90 / p99 / max.

The number that matters: for real FAST_CUSTOM Pump txs, first useful
OrbitFlare packet → exact publish.

First real FEEDCAP1 run: `docs/benchmarks/wire_to_state002.md`.
FAST_CUSTOM n=0. FEC-recovered n=0. Concat path is not the live Pump path.

## Tests

```text
./build/test_assemble
./build/test_capio
```
