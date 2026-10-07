# ANCHOR-001

gRPC/RPC is **bootstrap and repair only**. OrbitFlare advances live Pump
state. External fetches are never on the trading hot path.

## Commitment (required)

An Agave bank at `context.slot = S` can still be **open** under
`processed` / `confirmed`: more transactions may land in slot S after
the RPC read. Then

```c
if (tx.slot <= pool->anchor_slot) skip;
```

would drop later same-slot OF txs that were not in the snapshot.

Bootstrap and DIRTY recovery are cold path. **Anchor only against a
completed bank: `commitment=finalized`.** Then `context.slot = S` is a
frozen historical slot, and `slot <= S` skip / `slot > S` apply is
exact. No intra-slot RPC cursor.

Finalization lag does not matter except that the OF buffer must retain
enough history to cover `finalized S` through live `S+N`.

## What `anchor_slot` is

```text
anchor_slot := getMultipleAccounts(..., commitment=finalized).context.slot
```

Requirements (else **do not arm**):

- same RPC response
- pool + base vault + quote vault + global_config all present
- finalized (completed) bank
- successful compact decode

It is not Yellowstone `GetSlot`, not a process-wide epoch, not
`minContextSlot`.

```text
OF BUFFERING STARTS
        ↓
finalized Pump snapshot @ S
        ↓
arm pool EXACT @ S
        ↓
OF buffer may already be at S+N
        ↓
replay pool-relevant txs with slot > S
        ↓
caught up
        ↓
live shred-driven state
```

Vault pubkeys may be discovered in a prior RPC. That slot is discarded.
Only the single coherent request containing pool + vaults + global
establishes `anchor_slot`.

## Per-pool

```text
Pump A @ 12     Pump B @ 20
tx.slot > 12    tx.slot > 20
```

Pools enter or re-enter independently. No global state epoch.

## DIRTY recovery

```text
pool DIRTY
   ↓
keep buffering OF
   ↓
finalized snapshot @ new S
   ↓
re-arm
   ↓
replay slot > S
   ↓
EXACT
```

## Tools

```text
python tools/anchor/test_anchor.py
python tools/anchor/snapshot.py --provider shyft --pools pools.txt --out configs/pools/seed.jsonl
./build/test_catchup
```
