# STATE-008

Authoritative pricing state must be complete and transaction-coherent.
FUNDED stays 0. `recover_pda3` is independent.

STATE-007 subscribed liveuniv **pool pubkeys**, RPC-fetched bins/vaults once,
and never put those dependencies on the Yellowstone stream. A READY flag on
that plane was dangerous: new LbPair + old BinArray, or new Pump pool +
old quote vault, can look SYNCED.

```
Shyft Yellowstone
      ↓
complete pricing dependency subscription
      ↓
transaction-coherent staging
      ↓
atomic pool publication
      ↓
SYNCED / RACE-SAFE
```

Process: `arb-state/shyft/state008.py` (replaces state007 as recon writer).

## Per-pool states

| flag | meaning |
|---|---|
| STATE_INCOMPLETE | a required quote-kernel account is missing |
| STATE_STAGING | a write arrived; companions not yet coherent |
| STATE_COHERENT | required write set published atomically |
| STATE_GAPPED | stream gap / missing companion after a mutation |

A pool cannot be AUTH'd (searchable/sendable) unless **STATE_COHERENT**.

Funded eligibility later: route RACE_READY ∧ every hop STATE_COHERENT.
Not armed.

## Required accounts

Pump: pool, base vault, quote vault, global_config (fee bps).
DLMM: LbPair, BinArrays covering `active_id ± K`, fee/vol fields on the pair.
Oracle and mints are subscribed when known; they do not block COHERENT
unless the kernel starts reading them.

## Universe generations

```
UNIV gen N  → derive complete set → subscribe → then a pool may become COHERENT
new pool    → gen N+1 → subscribe its deps BEFORE it can be state-ready
```

## Staging

Stage by `(slot, txn_signature)` when Yellowstone provides it.
Otherwise wait for a later slot before publish.
Never publish a partial write set.

## Shadow (hard gate)

Tail PAPER `opp_synced.jsonl` `s_prime`. A sample is scored only when
the next coherent Yellowstone publish is **the same transaction**
(`txn_signature == sig_hex`). Boot AUTH and unassociated writes do not
count as exact and do not consume the prediction.

Bit-exact. Not close enough.

| venue | required equality |
|---|---|
| Pump direct | predicted S' == next coherent S (reserves) |
| DLMM direct | active_id, volatility, every touched bin x/y, fee state |

Mismatch writes `captures/state008/mismatch/<sig>.json` and is classified:

`kernel_bug` · `missing_transaction_local_write` · `wrong_transaction_association`
· `stale_preceding_state` · `fee_volatility_timing` · `bin_traversal_account_coverage`
· `fork_order` · `trigger_decode_error` · `state_missing`

Do not silently resync a mismatch into success.

First recorded line `active_id / volatility / bin:2804` on idx=26
(`81ec9fa0…`, paper `auth_slot=450474933`, publish `slot=450476741=bootstrap_slot`)
is **wrong_transaction_association**, not a kernel error. See
`FIRST_MISMATCH.json`.

First *associated* compare (`3c92c4f3…`, same pool): paper `s_prime.active_id=2804`
from `auth_slot=450474933` vs Yellowstone `2813→2812` at slot `450479081`.
That is **stale_preceding_state** (paper S ~4100 slots behind), not a kernel
bug. Stale samples are bundled and excluded from exact/mismatch rates.

Soak until hundreds/thousands of associated direct-exact transitions.
RACE_READY only where VECTOR_READY ∧ STATE_COHERENT. FUNDED stays 0.

## Telemetry (`~/captures/state008/METRICS.json`)

`shadow_total` `shadow_exact` `shadow_mismatch` `shadow_state_missing`
`shadow_trigger_decode_error` `pump_exact_pct` `dlmm_exact_pct`

mismatch fields: `active_id` `volatility` `bin_liquidity` `reserves`
`fee_state` `ordering`

Also: dependencies_expected/subscribed/received, pool_complete,
staging_latency, coherent_publish_latency, gap_count.
