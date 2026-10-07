# WIRE-TO-STATE-001

OrbitFlare packet → certified Pump publish. Not a validator. Not DLMM.

## Path

```text
packet_rx
 → tx_complete          (DATA_COMPLETE shred, first framed tx)
 → OrderedTx            (legacy / v0, ALT = FALLBACK)
 → key/account resolve
 → deps_classify
 → prestate certificate (POOL_AUTH_READY)
 → overlay / apply
 → exactness cert
 → atomic publish
```

Incomplete AUTH (`virtual_quote` / fees / vaults / mints not certified)
is `FALLBACK`. The engine does not claim exactness.

## Measure

```text
./build/wire_to_state_bench
```

Reports p50 / p90 / p99 / max per stage, FAST_CUSTOM vs FALLBACK,
exact-state e2e, fail reasons, how often tx bytes beat prestate,
and reconstruction vs certification vs apply budget.

Intra-process rdtscp. Not NIC RTT. FEC spanning txs are `tx_incomplete`.

Measured (synthetic complete shred, 2026-09-26): exact e2e p50 **891 ns**,
p99 **1.9 µs**. See `docs/benchmarks/wire_to_state.md`.

## Tests

```text
./build/test_decode
./build/test_wire
```

Next: `docs/wire/WIRE-TO-STATE-002.md`. DLMM still blocked.
