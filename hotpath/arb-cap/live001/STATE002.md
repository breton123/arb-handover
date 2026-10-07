# STATE-002 — canonical S vs speculative S'

```
canonical S  --hot_decide-->  temporary S'  --cycle_size-->  opportunity
                     |
                     +-- does not write S, does not bump version

confirmed N  --hot_commit-->  apply on canonical S, state_version++
```

`opportunity_t` is unchanged. The journal field lives on `hot_decision_t`:

```
opportunity used state_version = 450085375
```

Against `liveuniv.bin`:

| step | version | S | note |
|------|---------|---|------|
| bootstrap | 450085375 | snapshot | |
| decide DLMM 0.1 SOL | stamps 450085375 | unchanged | gp=6375724 |
| commit that N | 450085376 | DLMM reserves moved | prior decision stale |
| decide again | stamps 450085376 | | gp=16148084 |
| commit Pump 0.1 SOL | 450085377 | Pump reserves moved | previous decision stale |

`hot_stale(u, used)` is the audit check.

```bash
./build/state002 arb-cap/live001/liveuniv.bin
```
