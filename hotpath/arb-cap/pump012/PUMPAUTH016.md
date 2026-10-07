# PUMP-AUTH-016

One Pump AUTH generation. C `pump_state_t` blob layout unchanged
(vaults + virtual + **resolved** lp/proto/creator bps). Extra config
lives on `last_s` for overlay. FUNDED=0. Frozen 20260925 soak is not
touched.

## AUTH

- vaults, `virtual_quote_reserves`
- `pool.creator`, `pool.coin_creator`, `pool.creator_fee_bps`
- FeeConfig PDA + GlobalConfig + base mint supply
- resolved tier/flat rates written into the existing blob

## Predict

Every Pump CPI in geyser execution order. `buy` is exact-out and **TX_EXACT**.
Unknown relevant CPI → fail closed, no S′. Terminal overlay → predicted S′.

## Soak

`STATE008_CAP=/data/bsc/captures/soak_pump016_20260926/state008`

Gate: Pump exact txs, 0 unexplained prediction/publication mismatches.
