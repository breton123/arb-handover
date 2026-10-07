# STATE-005

Authoritative DLMM state is `LbPair` + real `BinArray` accounts. Vaults are inventory only and never seed `reserve_*`.

```
bootstrap RPC → pricing snap (bin-sum) → compact S → SYNCED

confirmed N → local apply → SPECULATIVE (not sendable)
            → control-plane LbPair + touched BinArrays
            → exact → SYNCED
            → mismatch → replace from chain + diagnose
```

Frozen `dlmm_apply_swap` is the only apply. ±16 cache is unchanged. `feed_live` stays up. PAPER stays off. No send.

## First real triple — AeWR4X9C — EXACT

Pool `GeUkx21V…` slot 450352241. `before.slot` 450352211 < tx. `Swap2Evt` 37→37, X→Y.

| field | before | apply | chain / event |
|---|---|---|---|
| active_id | 37 | 37 | 37 |
| vol_acc / vol_ref / idx_ref | 0 / 0 / 37 | 0 / 0 / 37 | 0 / 0 / 37 |
| last_upd | 1790112549 | 1790112549 | 1790112549 |
| amount_out | | 238,239,867 | 238,239,867 |
| fee / protocol | | 16,488 / 1,648 | 16,488 / 1,648 |
| bin 37 x | 2,076,917,222 | 2,241,780,378 | 2,241,780,378 |
| bin 37 y | 306,357,881 | 68,118,014 | 68,118,014 |

Window bin-sum: `Δrx = ain − fee`, `Δry = −out`. Vault X rose by full `ain` (fee stayed in the vault). `reserve_* ≠ vault_*`.

## Exclusion classes (not model failures)

| class | meaning |
|---|---|
| `clean` | singleton: `before.active == event.start`, apply bit-exact vs event + after bins |
| `interleave` | after snap is `after(N + other swap)` — bins/untouched moved; not a kernel fail |
| `stale_before` | RPC before-snap already past `event.start_bin_id` |
| `stale_after` | after snap is not `event.end` (or apply saw a later account) |
| `model_mismatch` | apply disagrees with `Swap2Evt` on a candidate singleton |

**GATE OPEN** 2026-09-25 12:34:47 UTC. `clean=13` including walk-1 `wLukjh4R` on `3sVrUSPn…` (1305→1304): active, vol, fee, out, both touched bins exact. `last_upd` still not written by the kernel (noted, not a fail).

Collector stays up for the 50–100 corpus. PAPER may reopen SYNCED-only; local apply is not trusted without refresh.

OrbitFlare / `feed_live` untouched. No send.
