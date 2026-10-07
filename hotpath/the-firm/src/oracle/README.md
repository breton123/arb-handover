# oracle

## Purpose

Compare our successor state to a certified reference (Firedancer / Agave
/ Yellowstone snapshot). Offline only.

## Responsibilities

- Field diffs (pool, token, system, incarnation, cert, overlay)
- Admission class (PUMP-ORACLE-001)
- Corpus load / replay

## Non-responsibilities

- RPC
- Running a validator
- Reconstruction / ALT resolution
- DLMM

## Testing

```text
./build/test_pump_oracle
```

See `docs/oracle/PUMP-ORACLE-001.md`.
