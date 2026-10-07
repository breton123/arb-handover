# Route closure — {DLMM, Pump}

Invariant:

**Supported venues ⇒ complete executable route closure.**

A venue is not “supported” until every typed closed 2/3-hop over the current set is searchable and executable. Frequency today does not choose the family. Invalid mint paths compile to nothing.

## Two-venue completion (before CPMM)

Search: all typed closed 2/3-hop over DLMM + Pump.
State: STATE-007 already subscribes liveuniv pools; those routes’ accounts are the set.
Execution: `ARBHOPS0` generic hop dispatcher — not OUR_EXEC, not ARBDLMM2.
Preparation: ALT/ATA plane must cover every compiled executable route (not done while #6 is armed).
Hot path target: FRAMED N → affected routes → quote → size gate → hops program → SWQOS.

No `family=255` and no `EXEC_MISSING` for a route whose hops are only DLMM/Pump.

## Family stamp

| family | meaning | executable |
|---|---|---|
| 0 | route0 denomination DLMM+Pump 2-hop | yes (OUR_EXEC today, hops later) |
| 5 | typed inverted DLMM+Pump 2-hop | yes (hops) |
| 6 | any other supported 2/3-hop | yes (hops) |
| 1–4 | includes an unsupported venue | no |
| 255 | not a supported-only closed route | no |

## Dispatcher

```
ARBHOPS0
hop_count = 2 | 3
amount_in, min_profit
hop[i]: proto, in_ata, out_ata, acc_n
for hop:
    DLMM → swap2
    PUMP → buy / sell
    else → fail
inventory profit guard Custom(6) on user[0]
```

Adding CPMM later is a new adapter in that loop, then the compiler exposes the full three-venue closure. Not “add some CPMM routes.”

## Isolation

ONESHOT #6, OUR_EXEC `38dsYLgt…`, STATE-007, RabbitStream: not changed.
`paper.c` / `hot.c` frozen — they still treat only FAM_0 as `signed_ready` / `opp.valid`. That is the remaining hot-path hole. Closure stamp + hops program are the contract. Wiring a new oneshot onto `ARBHOPS0` is the next control-plane step after Custom(6) on a **new** program id.

## Live graph (see CLOSURE.json)

`closure_gate.py` counts typed closes. A sequence at 0 means the mint graph has no such cycle, not that we refused to implement it.
