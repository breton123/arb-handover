# LIVE-STATE-001

Shadow-only live Pump CompactState. No quoting, no sending, no DLMM,
no extra programs.

AUTH never writes or gates CompactState. It is an independent referee
for the same signature after the live apply has already published.

```text
OrbitFlare ──> buffer/capture ──> frame tx
                      │
Shyft snapshot ──> arm pool @ finalized anchor_slot
                      │
                      ▼
               catch up from OF  (slot > S)
                      │
                      ▼
                LIVE CompactState
                      │
                predict S_after
                      │
                      ├──────────────> state continues immediately
                      │
                      ▼
             AUTH verifier (later)
                      │
               compare same tx
```

## Verification record (every successful apply)

```text
slot, signature, pool_id, incarnation
prestate_hash, predicted_poststate
of_rx_tsc, state_ready_tsc
```

`--pred FILE` writes one jsonl object per prediction.

## AUTH compare

Integer equality. No rounding tolerance. Fields consumed by
`pump_quote_exact_in` / apply:

```text
reserve_base, reserve_quote, virtual_quote
vault amounts
lp / protocol / creator fee bps (when AUTH carries them)
disabled / status (when AUTH carries them)
```

One MISMATCH:

```text
pool DIRTY immediately
dump framed tx + prestate + predicted + AUTH identities
```

`--dump FILE` is the mismatch log (keep as a regression fixture).

## Outcomes (keep distinct)

```text
MATCH  MISMATCH  AUTH_INCOMPLETE
NO_PRESTATE  LUT_MISS  UNSUPPORTED  GAP
```

Incomplete AUTH must not raise exact rate.
Correctness = MATCH / (MATCH + MISMATCH)
Coverage = applied / Pump-relevant framed txs

Passing a live campaign: zero MISMATCH across a large sample (tens of
thousands of independently verified transitions), many pools and hours,
with every other outcome counted under a fail-closed reason. Finite
runs cannot prove forever-100%; they can earn enough trust to let a
searcher consume EXACT later. That decision is not this milestone.

## Run live (production stream, OrbitFlare `:20001`)

This process **binds** `:20001`. Stop any other OF recorder on that port
first.

```text
./build/live_state_001 --live --bind 0.0.0.0 --port 20001 \
    --pools configs/pools/seed.jsonl \
    --disc run/pump_disc.txt \
    --luts configs/luts/warm.jsonl \
    --pred preds.jsonl --dump mismatch.txt
```

Stderr prints a one-line dashboard every second (applied, MATCH,
MISMATCH, GAP, UNSUPPORTED, NO_PRESTATE). SIGINT dumps the full
board. Without `--pools` you will see framed/Pump counts and
NO_PRESTATE; that is stream health, not EXACT.

`--disc FILE` appends unique LUT-resolved Pump pool pubkeys (hex).
Feed that file to `tools/anchor/snapshot.py --pools` then restart with
`--pools configs/pools/seed.jsonl`. Catch-up is `tx.slot > anchor_slot`
on the continuing OF stream (and on a later `--cap` replay of the
buffer). `have_meta=0` on reloaded learned LUTs stays uncertified.

## Run replay

```text
./build/live_state_001 --cap OF.cap \

    --pools configs/pools/seed.jsonl \
    --luts configs/luts/warm.jsonl \
    --auth writes.jsonl \
    --pred preds.jsonl \
    --dump mismatch.txt
```

Without `--auth`, MATCH stays 0 and incompletes are listed. That is not
a match claim. Without `--pools`, do not call the run live-exact.

`./build/test_livestate` covers MATCH, AUTH_INCOMPLETE, MISMATCH→DIRTY,
NO_PRESTATE.

Latency: first OrbitFlare packet TSC of the slot (until entry hits carry
per-tx rx) → state-ready TSC after apply.
