# WIRE-TO-STATE-003

Reed–Solomon FEC recover + Merkle-root check. Not Pump, AUTH, or DLMM.

## Path

```text
shard
 → enough (n data-or-code)
 → RS recover missing data
 → rebuild Merkle tree, match signed root
 → emit recovered data shreds
 → DATA_COMPLETE concat (index order)
 → Vec<Entry> incremental parse
 → tx_complete
```

Fail closed on conflicting signatures, roots, or shard bytes.

## Measure

```text
./build/test_rs && ./build/test_merkle && ./build/test_fec && ./build/test_stream
./build/rs_bench
./build/wire_to_state003 --cap /data/bsc/captures/orbitflare/FILE.cap
```

Measured: `docs/benchmarks/wire_to_state003.md` (SLOT-STREAM-002 Frankfurt).
