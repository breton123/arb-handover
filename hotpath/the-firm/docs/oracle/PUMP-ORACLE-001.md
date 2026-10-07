# PUMP-ORACLE-001

Prove the custom Pump engine against certified inputs. Not a validator.
Not DLMM. No RPC on the hot path.

## Inputs (per case)

1. Certified prestate (pool + watched token/system rows)
2. Exact `OrderedTx` (decoded legs, not raw shreds)
3. Reference-bank committed poststate (geyser `published_s` when `source=geyser`)

## Compared fields

- Pump reserves, virtual quote, fee bps
- Vault and user token amounts, WSOL native reserve, live flag
- System lamports
- Incarnation
- Publication certificate (slot, tx_index, sig, model, program, exactness)
- Terminal overlay vs certified post

## Classes

| class | meaning |
| --- | --- |
| `FAST_CUSTOM_EXACT` | complete pre, apply OK, every compared field matches |
| `FALLBACK_REQUIRED` | incomplete pre, or DLMM/runtime write, or reject FALLBACK |
| `UNKNOWN` | unknown relevant write (disc, Token-2022 extensions) |
| `UNEXPLAINED` | complete pre and we still disagree with the bank |

Acceptance: **0 UNEXPLAINED** on the held-out corpus.

Incomplete AUTH (missing virtual quote) is `FALLBACK_REQUIRED`. Do not
treat a quote miss as exact. Do not invert uninvertible exact-out buys
to inflate coverage.

## In-repo corpus

`tests/fixtures/oracle/pump-oracle-001.txt`

Geyser-anchored rows come from the frozen state008 SCOREBOARD examples
(slot 450505766 / 450503433). Exact-out invert is not unique on quote,
so overlay-016 composition is gated in `test_pump_corpus`, not claimed
as geyser-exact here.

The full 4118-file soak tree is not in this repository. When that tree
is mounted, import mismatch JSON into this text format; do not change
admission rules.

Scale import is **PUMP-ORACLE-002** (`tools/oracle/import_soak.py`,
`test_pump_oracle002`). Do not change these admission rules when
expanding the corpus.

A case without complete certified virtual/fee/config is `pre_complete=0`.
A case whose inner program is not a modeled Pump/token/system leg is
`FALLBACK_REQUIRED` or `UNKNOWN`.
