# Five DIRECT_PUMP txs (orbitflare-20260926-224724)

Same cap as WIRE-CLASSIFY-001. `--dump-pump`. FAST_CUSTOM stayed 0.

All five hit **Pump bonding** `6EF8rrect…` (`0156e0f6…`), not PumpSwap AMM `pAMMBay…` (`0c14defc…`).

| slot | enc | pump ix | disc | name |
| ---: | ---: | ---: | --- | --- |
| 450813520 | legacy | 4 / 5 | `33e685a4017f83ad` | bonding sell (same 8 bytes as AMM sell) |
| 450813523 | v0 | 3 / 5 | `38fc74089edfcd5f` | bonding, not buy/sell |
| 450813585 | legacy | 2 / 5 | `33e685a4017f83ad` | bonding sell |
| 450813834 | legacy | 3 / 5 | `33e685a4017f83ad` | bonding sell |
| 450813993 | v0 | 4 / 7 | `33e685a4017f83ad` | bonding sell |

Typical outer list: compute-budget, System transfer, ATA, **bonding**, Token close/sync.

Pump is never `ix[0]`. Classifier is right; `ingress_decode` only tags `PROTO_PUMP` on the AMM program id, so FAST_CUSTOM=0 is **correct**. These are not missed PumpSwap exact-in/out swaps.

Do not admit bonding into FAST_CUSTOM. Different state (virtual bonding vs AMM reserves).
