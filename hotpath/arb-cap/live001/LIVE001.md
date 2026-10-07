# LIVE-001 — real universe / state

Control plane: `arb-cap/live001.py` (RPC, outside arb-core).
Live binary: `arb-core` `live_univ_load()` only. No `universe_seed`. No synthetic S.

Snapshot slot **450085375** (`state_version = slot`).

```
7 pools   3 DLMM   4 Pump   2 compiled routes
```

Route0 pair (same SOL-sided token):

| idx | proto | pubkey |
|-----|-------|--------|
| 0 | DLMM | `DAErPzgiYQDhDwpMRgdvikXaZmnmznc3du3jy9i1AABk` |
| 1 | Pump | `BRjMA8UALNp3diTKHfdeAn5riQrYPoaYez7KhYWD4oEs` |

`routes_by_pool[0] = 2`, `routes_by_pool[1] = 2`. Other pools have no counterpart → 0 routes.

## Proof: real S → frozen kernels

All 7 quotes used `dlmm_quote_exact_in` / `pump_quote_exact_in`. 0.01 SOL in.

| idx | result |
|-----|--------|
| 0 DLMM | out=41677 |
| 1 Pump | out=2489100170 |
| 2–3 DLMM | ok |
| 4–6 Pump | ok |

`quoted  dlmm_ok=3  pump_ok=4  fail=0`

## What this is / is not

Is: dense `pool_idx`, real mints/vaults/bins/reserves/fees, `pool_table` lookup, 2/3-hop compiler on **those** mints, `state_version`.

Is not: HOT-001 (still need to wire `cycle_size` into the hot path), live apply of N, nonce/TPU, capital.

```bash
python arb-cap/live001.py --out arb-cap/live001/liveuniv.bin --sig <dlmm-pump-tx>
./build/live001 arb-cap/live001/liveuniv.bin
```
