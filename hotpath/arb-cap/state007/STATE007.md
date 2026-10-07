# STATE-007

Authoritative account-state stream for the Solana searcher.
FUNDED remained 0. No arb was sent.

## Architecture

Independent process `arb-state/shyft/state007.py` on the Frankfurt box.

```
Shyft Yellowstone gRPC RX thread
    → T_STATE_RX = CLOCK_MONOTONIC_RAW (before decode)
    → SimpleQueue
decode / stage thread
    → UPDATING (recon INVALIDATE) immediately
    → slot-or-txn coherent publish
    → recon AUTH (existing state_refresh path)
async journal thread
    → ~/captures/state007/updates.jsonl
```

Search / PAPER never calls gRPC or RPC. It only drains `recon.bin`.
Lookup stays O(1) `pool_idx → immutable S`.

Endpoint/region (no credentials):

- host: `grpc.fra.shyft.to`
- region: Frankfurt (FRA)
- commitment: PROCESSED
- auth: `x-token` metadata (never logged)

Subscription is built from the published liveuniv generation (171 DLMM/Pump
pools). Roles: `dlmm_lbpair`, `dlmm_binarray`, `pump_pool`,
`pump_vault_base`, `pump_vault_quote`. CPMM/DAMM/CLMM/Orca roles are reserved
on the same map. Universe diffs are control-plane only.

## Bootstrap algorithm

1. Open Subscribe (accounts + slots).
2. Buffer account updates. Wait until `stream_slot > 0`.
3. RPC `getMultipleAccounts` (processed). `bootstrap_slot = context.slot`.
4. Replay buffered updates with `(slot, write_version)` newer than bootstrap.
5. `STATE_READY` only if the stream stayed up and `stream_slot >= bootstrap_slot`.

Otherwise `STATE_NOT_READY` / SEND BLOCKED. Continuity is never assumed.

## Coherency algorithm

A relevant write marks the pool UPDATING (`state_sendable = false`) before
canonical publish.

- If Yellowstone provides `txn_signature`, stage by `(slot, txn_sig)`.
- Else conservative slot staging: publish only after a later slot arrives so
  same-slot companion accounts (LbPair + BinArrays) are in.
- If LbPair `active_id` moves and a required BinArray is missing, stay UPDATING.

Frankenstein (LbPair from N, BinArray from N-1) is refused.

There is no wall-clock TTL and no 32-slot TTL on the new predicate.

## Hot path

PAPER runs **both** predicates on every FRAMED candidate:

- old: `age32`
- new: `mut_authoritative` = plane READY ∧ every hop SYNCED ∧ not UPDATING

Production sendability is still the old #6 path. It was not switched.
ONESHOT is disarmed. `run_oneshot.sh` has `FUNDED=0`.

STATE-006 LAND/REJECT remains. Periodic RPC AUTH refresh is skipped once
`~/captures/state007/READY` exists.

## Subscription counts (this generation)

- pools: 171
- initial account keys: 171 (pool pubkeys; bin arrays / vaults added at bootstrap)
- stream READY: no

## Latency percentiles

Not measured. Zero account updates were delivered (stream never authenticated).

## Stream reliability

- `grpc.fra.shyft.to` is reachable (not UNAVAILABLE).
- `GetVersion` / `Subscribe` return `UNAUTHENTICATED` for every tried header
  (`x-token`, `x-api-key`, `authorization`, `access-token`, `token`).
- reconnects: 30+ in the first minutes, fail-closed each time.
- gaps: 30 (disconnects; no silent trust).
- updates: 0

The 16-character value in `SHYFT_KEY` is accepted as a token length but rejected
by Shyft gRPC. Yellowstone `x-token` is issued from the gRPC section of the
Shyft dashboard and is a different secret from the REST API key. Put that
x-token in `~/.arb-state007.env` as `SHYFT_X_TOKEN` (never commit it) and
restart `state007.py`.

## PAPER funnel (30 min, plane READY)

```
actual-size > hurdle       2
old age32 pass             0
mut_authoritative pass     1
would_send_new             1   877575 lamports
exec_fam (route0)          1   2482975 lamports  (pre-READY / not mut_auth)
opp_suppressed_UPDATING    0
```

age32 rejected 100% of economically eligible cycles. Shyft recovered one.

## would_send_new candidate (idx 161, HTvj…, dlmm-dlmm-pump)

```
every hop SYNCED            yes  3/3
no UPDATING                 yes
unresolved mismatch         no   (idx 161 never in compare-mismatch set)
YS/bootstrap generation     yes  gen=832  s007_ready=1
CORE010 FRAMED              yes
actual 0.05 quote           877575
RACE_READY / exec family    NO   3-hop, family=255, no ALT templates
```

This is the first mutation-authoritative +EV. It is not the first
production-contract send: there is no 3-hop executor.

The 2.48M route0 row is fatter but was `s007_ready=0` (pre-READY).
Wait for that shape with mut_authoritative before ONESHOT #6.
First funded floor is 2_000_000 lamports actual-size gross.

## Compare `mismatch=47` (later 62)

Lifetime counter, not 47 broken pools. The compare loop only samples
univ idxs 0–5 every 20s. 5 unique idxs, 62 repeats. Almost all
disagreements are hot-window bin amounts; some `active_id`/`vol_acc`.

That is RPC snapshot vs Yellowstone race on the most active DLMMs.
Compare now classifies `rpc_lag` when `rpc_slot < stream_slot` and does
**not** invalidate those. Same-slot disagreement still SEND BLOCKS.

## Production predicate

PAPER / oneshot send freshness is now `mut_authoritative`.
age32 remains journaled only. FUNDED=0. ONESHOT not armed.

## Remaining blockers

1. A route0 / ALT RACE_READY cycle that is also mut_authoritative.
2. Actual-size gross ≥ 0.002 SOL for the first funded race.
3. Same-slot compare mismatches should stay rare after rpc_lag split.

No venues. No DLMM³ executor. No arb sent.

STATE-007 FAIL — FUNDED REMAINS BLOCKED
