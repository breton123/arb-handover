# TX-OVERLAY-001

Model the whole transaction-local state transition, not just decoded swap N.
Canonical STATE-009 AUTH is untouched until Yellowstone confirms.

```
S_before                 ← STATE-009 predecessor (ring, not age)
   ↓
instruction 0 … k-1      ← overlay, in order
instruction k = swap N   ← exact kernel
later ixs                ← overlay or fail closed
   ↓
S'_tx = overlay
   ↓
Yellowstone committed S  ← shadow only if TX_EXACT
```

## Eligibility

`PREEXEC_EXACT` is split:

| flag | meaning | funded |
|---|---|---|
| `IX_EXACT` | one swap decoded exactly | no |
| `TX_EXACT` | every pricing-relevant effect on the watched pool is known | only this, later |

A perfectly decoded Pump swap plus an unknown same-pool instruction is not exact.
Other-pool unknown does not poison this pool's `TX_EXACT`.
Anchor event disc `e445a52e51cb9a1d` is not pricing.

Auth age 1–8 (or 100) is not a reject. The per-pool ring already guarantees
no relevant write between AUTH and N. Do not recreate the age-32 gate.

## First associated mismatch

Signature (b58):
`2sjGjP4KjtwZA8cQXTHJvPbFbhSmW76H1e8CZEoLAwE2TvZqK4QC4Cnas4Y8Y9QZfWAQngUmtXchrfjCiv5Qguup`

Slot `450482164`, err=null, 6 outer / 4 inner. Logs: `Swap`.

Outer pricing: one `dlmm_swap1` (`f8c69e91e17587c8`) on
`GYqytuXSiX3GaPuCkLMY4jeRR3mAtyAuQqhD32d45g5y`.
Inner DLMM: two Anchor events only.

Accounts include pair + bin array `5HUsU653znmCuEqK9rk6N3D1VsBCpYVuhKaLZ1GpPZ4v`.
YS `staged_writes` at publish time: **LbPair only**. `touched_bin_arrays=[]`.

`s_before == published_s` (active 2727 unchanged). Kernel predicted bin 2727
liquidity `x=516034070 y=283813495`. Shadow: `missing_transaction_local_write`
/ `bin_liquidity`.

### Which write exists on chain but not in the modeled transition?

The **BinArray** write `5HUsU653znmCuEqK9rk6N3D1VsBCpYVuhKaLZ1GpPZ4v` from
the **same** swap1 — not a second swap, not add/remove/rebalance, not a
router preceding leg, not a special DLMM instruction.

STATE-008 published as soon as the pair was complete, before the same-sig
bin companion arrived. Overlay of N is the full modeled transition for this
tx. The publisher had not yet staged the bin the swap wrote.

Fix: wait for a same-`txn_sig` `dlmm_binarray` (or the next stream slot)
before a tx-associated DLMM publish.

## Soak

100+ associated direct/exact transactions.

`S_before` (STATE-009) + transaction-local overlay = Yellowstone committed S.

Pump bit-exact. DLMM bit-exact. Unknown mutation → fail closed.

FUNDED stays 0. ARBHOPS0 untouched.
