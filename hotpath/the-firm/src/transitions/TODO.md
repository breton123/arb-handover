# Transition TODOs

## Done in this slice

- Pump sell, buy_exact_quote_in, buy exact-out
- Token amount/mint/owner/native/incarnation/program
- transfer, SyncNative, ATA create/init, close, system transfer
- Publication certificate
- Offline oracle replay vs certified post-state

## Token-2022

- Fail closed while `extensions_mask != 0`.
- Model transfer-fee / interest-bearing only with a frozen corpus.

## Runtime fallback

- `DEP_FALLBACK` must not become identity.
- Later: off-path Firedancer/Agave replay filling certified post-state.
- Never call RPC from `apply_tx`.

## PUMP-ORACLE-001

Held-out corpus in `tests/fixtures/oracle/pump-oracle-001.txt`.
PUMP-ORACLE-002 scale: 4118 usable, 0 UNEXPLAINED.
WIRE-TO-STATE-001: `docs/wire/WIRE-TO-STATE-001.md`.
WIRE-TO-STATE-003: `docs/wire/WIRE-TO-STATE-003.md`. Not DLMM.
Bar: 0 UNEXPLAINED. Do not start DLMM before that.

## DLMM (after Pump + token lifecycle is clean)

- Bin math, variable fee, active_id, order inventory.
- Overlay: repeated bin visits use provisional bins.
- Fail closed on Token-2022, mayhem, or incomplete bin set.

Blocked on: a held-out DLMM corpus and CompactState bin storage.
