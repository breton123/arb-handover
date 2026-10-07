# arb-core

What these bytes mean, and whether there is money.

```text
arb-nic     what bytes arrived?          →  rx_packet_t
arb-core    what does this packet imply? →  speculative S'  →  is there money?
later       turn that edge into a landed tx
```

This tree stays tiny. No validator, no ledger, no RPC, no generic SDK on the hot path. Replay today; DoubleZero / AF_XDP later. Execution is a later tree.

```text
35k recorded shreds
        ↓
arb-core replay
        ↓
shred_view          frozen
        ↓
AVX2 relevance      LOCKED
        ↓
CORE-001            relevant shred → pool_idx   LOCKED
        ↓
CORE-002            offset recovery + trigger recall   measured
        ↓
CORE-003            DLMM trigger → predicted state     FROZEN
        ↓
CORE-004            compact cache + real-S validation  FROZEN except --cap
        ↓
CORE-005            predicted S' → quote               FROZEN
        ↓
CORE-006            PumpSwap apply + quote             FROZEN
        ↓
CORE-007            fixed-size DLMM ↔ Pump cycle       FROZEN
        ↓
CORE-008            size → opportunity_t               FROZEN  core boundary
```

**arb-core v1 is frozen.** `opportunity_t` is the only thing that leaves this tree (gross protocol profit). Do not optimize CORE-008. Do not add landing, tips, or an executor. Do not grow the DLMM cache. Execution is `arb-exec`.

## CLASSIFY-002 — locked

AVX2 reject is the production path (~126 cyc). AVX-512 lost. Do not retune.

## CORE-001

Relevant shred only. Recover the static account-key list that contains the program id, take DLMM/Pump instruction account[0] as the pool candidate, look up a compact `pool_idx`. No VersionedTransaction, heap, base58, FEC, or full instruction decode.

Universe is built from the same recorded shreds (`build_pools`).

```bash
cd ~/arb-core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/build_pools ~/arb-nic/data/shreds.arbrx data/pools.bin
./build/core001 --file ~/arb-nic/data/shreds.arbrx --pools data/pools.bin --loops 8 --warmup 35512 --cpu 47 --mlock
./build/core002 --file ~/arb-nic/data/shreds.arbrx --pools data/pools.bin --loops 8 --warmup 35512 --cpu 47 --mlock
python3 scripts/trigger_recall.py data/triggers.json ~/arb-nic/data/shreds.arbrx
```

CORE-001 is frozen. CORE-002 runs only after the fast path misses. Score trigger recall, not shred recall. The 431-slot Mriya labels (slots ~448.3M) do not overlap this TVU dump (slots ~449.6M).

## CORE-003

`dlmm_apply_swap(before, ix, after)` — exact-in, SPL, no limit orders, no Token-2022. Math from the vendored Meteora commons SDK (fee, volatility, Q64.64 bin price, bin walk). Stores only fields needed to reproduce the transition.

```bash
cd ~/arb-core
./build/core003
```

Live `before N / tx N / after N` account triples are not in the TVU dump. Synthetic protocol cases run now. A recorder for historical triples is the validation expander, not more networking.

CORE-003 is frozen. Do not change `dlmm_apply_swap`.

## CORE-004 — locked (cache + validation only)

Done means: given a **real** DLMM state `S` and a **real** incoming swap `N`, the compact row `pools[pool_idx]` either reproduces the economically relevant `S'`, or fails closed because a required bin is absent.

Trigger N never writes canonical S. Missing bin → FAIL. Window is `active_id ± 16` until the live corpus says otherwise.

The validation corpus reports exactly three things:

1. prediction accuracy — `predict(S,N) = S'`
2. state sufficiency — bins crossed / required cached bytes (p50 p90 p99 p99.9 max)
3. cycles as a function of bins crossed — 0 / 1 / 2–4 / 5–8 / 9+

```bash
cd ~/arb-core
./build/core004 --cpu 47 --mlock
./build/core004 --cap data/dlmm.cap
```

Synthetic self-check is not the corpus. `getTransaction` / shreds / `snapshot-dlmm.json` (one slot, 98 accounts) do not contain `(S, N, S')`. A recorder outside this tree writes `.cap`.

## CORE-005 — locked

`dlmm_quote_exact_in(S', x, d)` — exact-in quote against a predicted state. Same walk as `dlmm_apply_swap` (apply on a copy). Does not write `S'`. No Pump, routes, size, or profit.

```bash
cd ~/arb-core
./build/core005 --cpu 47 --mlock
```

`quote(S, x)` amount_out/fee must equal `apply_swap(S, x)`. `S` is bit-identical after quote. Missing bin → fail closed.

CORE-005 is frozen. Do not change `dlmm_quote_exact_in`.

## CORE-006 — locked

`pump_apply_swap` / `pump_quote_exact_in` — PumpSwap exact-in, both directions. Constant product on `(base, quote + virtual_quote)`. Fees `ceil(n * bps / 10000)` (LP + protocol + creator). Quote is apply on a copy. No routes.

Restricted: SPL, no Token-2022, no mayhem/cashback, rates already resolved (no live fee-tier walk). Unsupported → fail closed.

```bash
cd ~/arb-core
./build/core006 --cpu 47 --mlock --vectors tests/pump-vectors.txt
```

Official SDK vectors (10k) must match `amount_out` exactly. `quote == apply`. `S` unchanged after quote. Live Pump triples are a later arb-cap job, not a gate.

CORE-006 is frozen. Do not change `pump_apply_swap` / `pump_quote_exact_in`.

## CORE-007 — locked

`cycle_quote(dlmm, pump, amount_in, direction)` — one fixed size, two quotes, `gross_profit = out − in`. Caller passes predicted S' for the trigger venue and canonical S for the other. Neither state is written.

Alignment (frozen): Pump quote = SOL, Pump base = TOKEN; DLMM Y = SOL, DLMM X = TOKEN.

- `0` SOL → DLMM → TOKEN → Pump → SOL
- `1` SOL → Pump → TOKEN → DLMM → SOL

Either leg fail-closed → cycle fail-closed. No sizing, routes, or exec.

```bash
cd ~/arb-core
./build/core007 --cpu 47 --mlock
```

CORE-007 is frozen. Do not change `cycle_quote`.

## CORE-008 — locked

`cycle_size(dlmm, pump, &opp)` — geometric SOL ladder (0.01…100), both directions, then 8-point refine in the best bracket. No alloc. `opportunity_t` is the core→exec boundary: route, direction, amount, **gross** protocol profit. No tips, Jito, or landing.

`valid = 0` if no positive gross. Tie-break: larger profit, then smaller `amount_in`. `route_id = 0` (the only route).

```bash
cd ~/arb-core
./build/core008 --cpu 47 --mlock
```

CORE-008 is frozen. Do not change `cycle_size` or `opportunity_t`.

## CORE-009 — universe expansion beside v1

Locked `classify.c` (2-ID AVX2) and `route0` stay frozen. New venues share one adapter contract:

```text
apply_swap(S, N, S')
quote_exact_in(S, amount, direction)
```

`classify_n` / `find_prog_id_n` know DLMM, Pump, CLMM, CPMM, DAMM v2, Orca. Offline compiler emits SOL-aligned 2-hop and 3-hop only into `routes_by_pool[]`. Hot path walks that list. Exec adds route families 1–4 without mutating route0.

```bash
cd ~/arb-core && cmake --build build --target core009 && ./build/core009
cd ~/arb-feed && cmake --build build --target cap009
./build/cap009 --dir /home/louis/captures --ids 6
```

Stop expansion if `actionable → signed` p50 leaves the ~20 µs class or if `hot_eval_pool` scales with global route count.

## LIVE-001 — real universe / state

Control plane (`arb-cap/live001.py`) fetches current mainnet DLMM + Pump accounts and writes `liveuniv.bin`. The live binary only loads that file. Frozen `dlmm_quote` / `pump_quote` run on real S. No `universe_seed`.

```bash
python3 arb-cap/live001.py --out arb-cap/live001/liveuniv.bin
cd ~/arb-core && cmake --build build --target live001
./build/live001 ~/TheMoneyMaker/arb-cap/live001/liveuniv.bin
```

## HOT-001 — real route0 economics

Incoming N → cached S → predict S' (no commit) → frozen `cycle_size()` → `opportunity_t`.
`amm2` cannot quote or size a route0 hop.

```bash
./build/hot001 ~/TheMoneyMaker/arb-cap/live001/liveuniv.bin
```

## STATE-002 — canonical S vs speculative S'

`hot_decide` evaluates on a temporary S' and stamps `hot_decision_t.state_version`.
`hot_commit` is the only writer of canonical S; it increments `state_version`.
`hot_stale(u, used)` is the audit check. `opportunity_t` is not widened.

```bash
./build/state002 ~/TheMoneyMaker/arb-cap/live001/liveuniv.bin
```

## Layout

```text
include/     shred, rx, classify, tx, pool, dlmm, dlmm_cache, pump, cycle, opportunity
src/         classify.c (LOCKED)  tx.c  pool.c  cycle.c
             dex/meteora_dlmm.c (FROZEN)  dex/dlmm_cache.c  dex/pump.c (FROZEN)
tests/       replay  build_pools  core001–core008  pump-vectors.txt
scripts/     trigger_recall.py  dlmm_cap.py
```
