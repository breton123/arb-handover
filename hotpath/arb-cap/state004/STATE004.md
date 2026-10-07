# STATE-004

`dlmm_state_t.reserve_*` is priced bin liquidity (`into_bin = included − fee`). SPL vault balances are not that quantity. Fees stay in the vault, outside the bins, until claimed.

Do not change CORE-003 apply math to force `reserve_* == vault`.

## Account state — exact on all 10 landed

Every successful corpus34 member decoded a `Swap2Evt`. Vault token balances match the event with no leftover:

```
vault_x_before + user_in − host_fee = vault_x_after
vault_y_before − user_out           = vault_y_after
```

(or the Y-in / X-out swap of those legs). Host fee PDA never moved. The only vault-touching transfers are user → input vault and output vault → user. Transfer fee: none (classic SOL/USDC).

| i | sig | dir | bins | user in | user out | mm / proto / host | vault in exact | vault out exact |
|---|-----|-----|------|---------|----------|-------------------|----------------|-----------------|
| 1 | mwSC5UAu | X→Y | −21127→−21129 | 4,363,488,457 | 527,509,908 | 646,198 / 71,798 / 0 | yes | yes |
| 2 | 5DDLLcMW | X→Y | −21135→−21136 | 1,315,832,588 | 158,924,702 | 355,275 / 39,475 / 0 | yes | yes |
| 6 | oGBwhTwK | Y→X | −21137→−21136 | 306,266 | 2,534,559 | 48 / 5 / 0 | yes | yes |
| 7 | yPZ4UkLz | X→Y | −21127→−21129 | 2,985,446,925 | 360,918,439 | 367,357 / 40,817 / 0 | yes | yes |
| 9 | 2iDSLUAn | X→Y | −21115 | 1,706,667,724 | 206,598,874 | 191,233 / 21,248 / 0 | yes | yes |
| 16 | 3FHKswXc | X→Y | −21080→−21081 | 2,450,474,458 | 297,639,143 | 353,972 / 39,330 / 0 | yes | yes |
| 18 | 2Kayka1J | X→Y | −21102 | 615,835,098 | 74,647,616 | 58,821 / 6,535 / 0 | yes | yes |
| 21 | 4iHyAbfR | X→Y | −21095 | 2,043,554,896 | 247,881,554 | 185,024 / 20,558 / 0 | yes | yes |
| 25 | iZEWwfHc | X→Y | −21122 | 814,840,367 | 98,570,518 | 92,115 / 10,234 / 0 | yes | yes |
| 32 | 2uVXrC8J | X→Y | −21135 | 1,519,367,823 | 183,547,909 | 245,529 / 27,280 / 0 | yes | yes |

Protocol share is 10% of total fee on every row. `fees_on_input` and `fees_on_token_x` are true on the X→Y swaps: the fee never leaves vault X.

## mwSC5UAu residuals

| | kernel (synthetic bins) | chain (event + vault) |
|---|---|---|
| active | −21121 → −21121 | −21127 → −21129 |
| fee | 1,309,047 (~30 bps) | 717,996 (mm 646,198 + proto 71,798) |
| amount_out | 527,808,049 | 527,509,908 |
| vault_x − kernel_rx | 1,309,047 | = kernel fee, not chain fee |
| vault_y − kernel_ry | 298,141 | = kernel_out − chain_out |

The 1.31M X gap is the kernel subtracting its own fee from a vault-seeded `reserve_x`. On-chain, 717,996 of fee actually sits in the vault outside bins. The extra kernel fee is a stale-vol / wrong-window rate (~30 bps vs the event’s ~16.5 bps), not a missing transfer.

The 298k Y gap is not a fee. Vault Y equals `vault_before − event.amount_out` exactly. The kernel over-out is the synthetic “all Y in one bin at −21121” curve, not accounting.

STATE-003’s “active_id −21121 exact” was a later `getAccountInfo` on the live pair, not the post-tx bin. The event is the authority.

## Price state — not proven on this corpus

| field | status |
|---|---|
| active_id | event only; historical pair was not −21121 |
| bins touched amount_x/y | no pre-swap bin arrays |
| volatility accumulator | no pre-swap `v_parameters` |
| fee state | kernel 30 bps ≠ event 717,996 |

`live001.snap_from_accounts` writes `reserve_*` from the SPL vault when the vault account is present, else from the bin sum. Those two meanings were already mixed at snapshot. Equality of `reserve_*` to vault is the wrong gate.

## Send

SEND BLOCKED remains. Account residuals are explained; pricing state is not exact. The next quote needs synced `active_id`, touched bin amounts, and fee/vol — not vault equality.

STATE-003 observe / confirm / reject is unchanged. Frozen ±16 cache is unchanged. No send.
