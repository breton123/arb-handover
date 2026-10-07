# PUMP-ORACLE-002

Corpus-scale Pump admission. Not a validator. Not DLMM. No RPC.

The oracle judges the whole declared Pump projection (reserves, virtual
quote, fees, overlay, cert) and is allowed to return `FALLBACK_REQUIRED`
or `UNKNOWN`. It must not claim `FAST_CUSTOM_EXACT` and be wrong.

## Import

```text
state008/mismatch/*.json
        + n015_tx (optional CPI/WSOL/Token-2022 tags)
        ↓
tools/oracle/import_soak.py --no-seed
        ↓
tests/fixtures/oracle/pump-oracle-002.jsonl
        ↓
FIRM_SOAK_ROOT=… ./build/test_pump_oracle002
```

Certified prestate is only what AUTH already recorded. Invert-recovered
`V` is counted as `invert(V) hits`, never written as `pre_complete=1`.

```text
python tools/oracle/import_soak.py --no-seed \
  --root /data/bsc/captures/soak_fastsoak_20260925/state008
```

## Scale result (2026-09-26)

Imported 4,118 Pump mismatch files from Frankfurt
`/data/bsc/captures/soak_fastsoak_20260925/state008`. Every AUTH blob
omitted `virtual_quote`, so `pre_complete=0` for the whole soak.

| | n | % |
| --- | ---: | ---: |
| usable | 4118 | 100 |
| FAST_CUSTOM_EXACT | 0 | 0.00 |
| FALLBACK_REQUIRED | 4118 | 100.00 |
| UNKNOWN | 0 | 0.00 |
| UNEXPLAINED | 0 | 0.00 |
| FAST_CUSTOM_EXACT but wrong | 0 | — |

| breakdown | n |
| --- | ---: |
| sell | 2588 |
| buy_exact_quote_in | 1427 |
| buy exact-out | 0 |
| multi-CPI | 178 |
| WSOL/system | 227 |
| association | 103 |
| virtual miss | 3847 |
| fee/config miss | 0 |
| invert(`V`) hits | 2537 |
| wrong-N | 168 |
| Token-2022 | 27 |
| unsupported branch | 0 |
| skipped | 0 |

`invert(V) hits = 2537` matches the old SCOREBOARD `explained_virtual`
count. Those rows are still `FALLBACK_REQUIRED`: recovered `V` is an
explanation, not a certified pre.

Acceptance: usable ≥ 1000, **UNEXPLAINED = 0**,
**FAST_CUSTOM_EXACT but wrong = 0**. Met. 100% FAST_CUSTOM was not
required.

## Classes

| class | meaning |
| --- | --- |
| `FAST_CUSTOM_EXACT` | apply OK/irrelevant and every compared field matches |
| `FALLBACK_REQUIRED` | incomplete AUTH (missing `V`/fees), assoc, wrong `n`, DLMM, or reject FALLBACK |
| `UNKNOWN` | unknown relevant write (disc, extensions) |
| `UNEXPLAINED` | complete pre and we still disagree with the bank |

## Next

`WIRE-TO-STATE-001` / `002`: `docs/wire/`.
DLMM stays blocked until live shred→publish numbers are trusted.
