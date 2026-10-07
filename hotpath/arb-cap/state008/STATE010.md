# STATE-010 — transaction-complete publication barriers

TX-OVERLAY-001 stays soaking. This ticket is publication, not the kernel.

```
Yellowstone transaction
        ↓
derive pricing write expectations
        ↓
account updates keyed by txn_sig
        ↓
all expected pricing writes seen
        ↓
atomic AUTH
```

`all dependencies exist` is not enough. Pair-complete used to AUTH
before the same-tx BinArray arrived.

## Expected write set (direct exact)

Intersection of the swap's accounts with quote-kernel state.

DLMM: LbPair + every BinArray referenced (remaining accounts / ROLE_DLMM_BIN).
Pump: pool + base vault + quote vault.

User ATAs, token reserves, Anchor events are not waited on.

```
sig X
expected = {pair, binA}
received = {pair}        → HOLD
received = {pair, binA}  → COMMIT
```

Next stream slot is only a fail-closed timeout:

```
next slot + expected incomplete
  → STATE_TX_INCOMPLETE
  → invalidate pool
  → do NOT AUTH
```

## 2sjGjP4… (the associated mismatch)

The kernel-state bin array in that swap is remaining-account
`8BMU2qbuTZJAp1BETTpCKu5zWZ3L9fFX8ys5rAP2w9kE` (index 38).
`5HUsU653…` is not a bin PDA for this pair.

Regression: pair-then-bin and bin-then-pair both leave AUTH unchanged
until the second write, then one new coherent generation, same S.

## Soak (TX_EXACT only)

`S_before + N_full_tx = S_Yellowstone`

Counters: `tx_exact_total/shadowed/bitexact/mismatch`,
`publish_wait_{pair,bin,vault}`, `publish_incomplete_at_boundary`,
`DLMM_exact_pct`, `Pump_exact_pct`.

Mismatches split `publication` vs `prediction`. FUNDED=0. Paper overlay
untouched. ARBHOPS0 still later.
