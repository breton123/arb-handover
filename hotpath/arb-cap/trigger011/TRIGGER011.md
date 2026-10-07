# TRIGGER-011 — Generic account-centric trigger coverage

FUNDED = 0 for the entire task. `classify_n` was not given FLASHX / DF1ow / 6Vo / Jupiter program IDs. No new venue. No invented S'.

## Verdict

**TRIGGER-011 FAIL — FUNDED REMAINS BLOCKED**

The detector is no longer outer-program-centric. The remaining blockers are exact router state transitions and live MEV.live predecessor recall, not “add these four program IDs.”

## Architecture (what shipped)

```
raw OrbitFlare shred
        │
        ├─ CORE-001 classify_n  (unchanged ultra-fast static DEX scan)
        │
        └─ generic frame_try_any          T_FRAMED
                │
                ├─ local ALT cache get     T_ALT_RESOLVED   (miss → ALT_MISS, no guess)
                │
                ├─ watched-account ∩       T_RELEVANT
                │         pubkey → {pool_idx, protocol, role}
                │
                ├─ swapix_from_loaded      T_N_EXACT
                │         program id may live in static / wr ALT / ro ALT
                │
                └─ else RELEVANT_UNKNOWN   amount_in = 0, no S', journal
```

States: `TRIG_DROP / INCOMPLETE / INVALID / ALT_MISS / EXACT / RELEVANT_UNKNOWN`.

CORE-001 / CORE-010 `frame_try_at` still requires a static DLMM/Pump program ID. `frame_try_any` does not. v0 instruction indexes may address ALT-loaded keys. INCOMPLETE retries as adjacent shred bytes arrive. No `Vec<Entry>`, no DATA_COMPLETE wait.

Hot path: memory only. RPC lives in `alt_plane.py` / `fetch_alts011.py`.

## 1. Framing decoupled from DEX detection

`arb-core/src/frame.c`: `frame_parse(..., require_dex)`. Fast path `require_dex=1`. Generic path `require_dex=0` and does not reject `prog_idx/acc_idx ≥ n_static` on v0.

Unit: ALT-only Pump sell is `FRAME` on `frame_try_any`, rejected by `frame_try_at`, `classify_n == REL_N_NONE`.

## 2. Local ALT cache

`alt_cache_t`: pubkey → ordered address vector. `get` returns miss, never synthesizes addresses. Misses enqueue via `alt_cache_note_miss`. Persist/reload: `ALT1` binary (`alt_cache_save` / `alt_cache_load`).

Off-path:

- `fetch_alts011.py` — late-121 LUT set + live misses
- `alt_plane.py` — tails `~/captures/trigger011/alt_miss.jsonl`, writes `alt_cache.bin`

PAPER reloads that file about once per second on the follow-idle path.

Prefetch of the late-121 LUT set: **71 unique ALTs, 66 fetched, 5 fail**.

## 3. Watched-account index

`watch_from_univ` indexes every liveuniv pool + `vault_x` + `vault_y` (role pool/vault). Live PAPER: **171 pools → 513 watched keys**. Bin/oracle/bitmap pubkeys are not in `dlmm_cache` so they are not invented.

A transaction is relevant iff the **resolved** account vector intersects this index, regardless of outer program.

## 4. Direct DEX recovery after ALT resolve

`swapix_from_loaded` walks the wire with `n_static` and resolves program/pool from the loaded vector. Supported: Pump sell / buy_exact_quote_in, DLMM swap / swap2. Pump buy exact-out still fail-closed.

Unit recovers ALT-loaded Pump sell: `amount_in=10000`, `pool_idx=7`.

## 5–7. Generic CPI + fail-closed unknown

No router allowlist on the detect path.

Resolved + watched + no supported DEX ix → `RELEVANT_UNKNOWN`. `amount_in` forced to 0. PAPER journals `kind=trigger011` and **does not** enter `close_candidate`.

Inspected highest-value historical routers (off-path `getTransaction`):

| outer | example | outer data | exact N? |
|---|---|---|---|
| 6Vo3245… | `2ppTmawN…` | 19 B `f0f3774c5c21c072…` first account = watched Pump `ENiVH49X` | **no** — amount is not in outer data (inner Pump moved 1.107e12 base / 26.16 SOL). Inventing from vaults or account order is forbidden. |
| DF1ow4ts… | `2b1NgbsW…` | 218 B, no embedded Pump/DLMM disc | **no** |
| FLASHX8D… | `F6GsarBc…` | RPC miss on this sig | **no decoder** |

So criterion 4 is not met: we capture these as `RELEVANT_UNKNOWN` and will not quote them.

## 8. Eventual execution feedback

Journal fields: klass, relevant, n_watch, alt_miss, nlut, n_loaded, pool0, outer_hex, alt_hex, sig_hex. This is the decoder-priority corpus. Landed inner-ix compare is the next off-path consumer; it is not wired into send.

## 9. Permanent late-121 regression

`python arb-cap/trigger011/hist_funnel011.py`

Previous CORE-010 static-DEX funnel: **38 FRAMED**.

| stage | N | $ |
|---|---:|---:|
| KNOWN REAL LATE ARBS | 121 | 419.8 |
| raw trigger available | 121 | 419.8 |
| generic FRAMED | 100 | 226.6 |
| ALT referenced | 51 | 160.6 |
| ALT resolved (legacy or cache hit) | 89 | 119.3 |
| watched-account relevant | 12 | 30.7 |
| direct exact N | 33 | 131.6 |
| router exact/predictable N | 0 | 0.0 |
| RELEVANT_UNKNOWN | 11 | 30.6 |
| ALT_MISS (no fake S') | 5 | 13.3 |
| unrecoverable / bad capture | 72 | 244.2 |

Remainder classes (every miss is classified):

- **21 invalid `nsig_range`** — shred fragment does not start at a real tx (bad historical capture)
- **51 unrecoverable FRAMED** — 47 ALT-resolved, 4 legacy; **no intersection with the current 171-pool liveuniv**. Detector saw the tx. Universe did not contain the pool. Not a favourite-pool trim — liveuniv is 171 < 256.
- **11 RELEVANT_UNKNOWN** — watched pool hit, no deterministic N (routers)
- **5 ALT_MISS** — LUT fetch failed; fail-closed
- **0 invented S'**

Framing recall 38 → 100 is the large move. Exact-N dollars are similar to the old static-DEX count because the fat dollars sit in router / out-of-universe / broken-capture buckets.

The 121 is a regression corpus, not the universe definition.

## 10. Live MEV.live hour

Reference hour (`arb-cap/regress/mev_hour.json`):

- successful DLMM/Pump: **2464 / $2085**
- all successful: **62696 / $6626**

PAPER now emits `trig gen / exact / unk / alt_miss` every second. Events append to `~/captures/trigger011/events.jsonl`.

Predecessor-level match against those 2464 winners still needs the slim-block predecessor pass used for late-121. That pass was **not** completed for this live hour, so criterion 6 is not met. `live_recall011.py` writes the live funnel + journal counts against the reference market size.

## 11. Universe storage

`live_univ_t` is `calloc`'d in PAPER (off-stack). `LIVE_POOL_MAX` stays 256 because the loaded univ is 171, not because we picked 256 favourites. If watched pools required by live + hist + market exceed 256, the next change is heap generations / pointer publish — not a popularity cap. Routes remain `routes_by_pool[changed_pool]`.

## 12–13. Latency / fast path

`classify_n` still runs first. Generic scan runs only on `REL_N_NONE`. `core010` still passes. `trigger_metrics_t` accumulates ns for frame / ALT / watch / decode (`CLOCK_MONOTONIC_RAW`). No RPC in that chain.

## Success criteria

| # | required | result |
|---|---|---|
| 1 | v0 ALT-loaded direct DEX recovered | **yes** (unit + loaded swapix + hist exact 33) |
| 2 | relevance = watched ∩ resolved keys | **yes** |
| 3 | unknown routers captured without allowlist | **yes** (11 hist + live `unk`) |
| 4 | highest-value routers → exact N where deterministic | **no** — 6Vo/DF1ow/FLASHX not deterministic from outer ix; no invented decoder |
| 5 | late-121 recall up; remainder classified | **partial** — FRAMED 38→100; exact $ not a new fat bucket; all remainders classified |
| 6 | live MEV.live predecessor explain | **no** — funnel instrumented, hour not predecessor-matched |
| 7 | ALT resolve local on hot path | **yes** (cache get only) |
| 8 | no fake S' for unknown CPI | **yes** |
| 9 | CORE-001 fast path preserved | **yes** (`classify_n` first, core010 ok) |
| 10 | not constrained to the 121 routers/pools | **yes** (generic frame + watch index + ALT learn) |

## What must happen before FUNDED can be reconsidered

1. Deterministic router decoders **after** relevance, only when outer ix + resolved accounts map onto the same normalized N as a direct Pump/DLMM swap. 6Vo is the first research target (pool is already account 0; amount is not in the 19-byte payload).
2. Slim-block predecessor recall for the live MEV.live hour: seen / framed / ALT / relevant / exact / unknown / missed, with dollars.
3. ALT plane hit rate near 100% on economically relevant live traffic (sidecar must stay up; 5 historical LUTs still missing).
4. Universe growth when watched-relevant FRAMED traffic exceeds the current 171, without a 256-favourite selection hack.

Until then FUNDED stays 0.

TRIGGER-011 FAIL — FUNDED REMAINS BLOCKED
