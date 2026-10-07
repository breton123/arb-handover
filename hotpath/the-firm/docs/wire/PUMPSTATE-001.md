# PUMPSTATE-001

Framed OrbitFlare transaction → PumpSwap terminal overlay.

```text
framed tx
 → txview_decode
 → txview_resolve (canonical v0 LUT order; fail closed)
 → txview_bind (Pump AMM discs only)
 → overlay apply of every supported ix
 → publish once
```

Bonding is not admitted. Unresolved LUT, unsupported pool instruction,
missing AUTH, or apply failure marks the pool **DIRTY**. No invented
state.

LUT cache: `configs/luts/warm.jsonl` (`key_hex` / `addrs_hex`). Research
coverage is a warm cache, not lifecycle certification.

## Tests

```text
./build/test_pumpstate
./build/pumpstate_001 --self-test
./build/pumpstate_001 --cap FILE [--luts configs/luts/warm.jsonl] [--pools FILE]
```

Exact-terminal compare is AUTHSTATE-001 (`docs/wire/AUTHSTATE-001.md`):
seed `--pools`, then reconcile `--auth` writes by `(slot, signature)`.

## Exact class

For every `PS_OK` / `predicted=1` result, terminal pool reserves must
match later AUTH. Empty bootstrap ⇒ do not claim live exactness.

Catch-up skip is `tx.slot <= pool->anchor_slot` only when that anchor is a
**finalized** completed bank (ANCHOR-001).
