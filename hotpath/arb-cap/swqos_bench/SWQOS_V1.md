# SWQOS V1 — READY pool + failover

Control-plane pool of 4 QUIC connections. Hot path never DNS, handshake,
or reconnects. Fail: mark DEAD, try one other READY, then `SEND_FAIL`.
A control thread reconnects DEAD → READY.

Frozen `leader_send` / route0 / CORE untouched.

## Gate (Frankfurt, tonight)

```
WARM  100/100 swqos_send  fail=0  ready stayed 4/4
                     p50      p90      p99      min      max  (µs)
sign                15.5     19.6     23.4     12.8     32.3
signed→SWQOS ret    10.1     20.3     22.3      6.3     23.8
```

`send_ns` is T1 (`exec_sign` return) → T2 (`swqos_send` return) on an
already-READY connection. No reconnect inside that interval.

The earlier ~47–56 µs COLD number was the old mpsc-worker hop plus a
fresh-enough conn. Direct `pick READY → open_bi → write → finish` is
~10 µs p50 / 22 µs p99 here. Still do not treat reconnect as free:
handshake stays on the control thread, off the opportunity path.

## Idle (same held pool)

| idle | send_ns | ready after |
|--|--|--|
| 1 s | 19.4 µs | 4 |
| 10 s | 21.6 µs | 4 |
| 30 s | 20.7 µs | 4 |
| 60 s | 20.3 µs | 4 |
| 300 s | 22.2 µs | 4 |

Keepalive 10 s / idle timeout 360 s. No idle-death in this window.
The first-night WARM die-after-4 was the two-conn worker with no
DEAD/READY manager, not a 1–5 min idle timeout.

## Policy

```
signed tx
  → READY A  (stream write+finish)
       ├ success
       └ fail → mark DEAD → READY B
                    ├ success
                    └ SEND_FAIL
```

No loops. No blocking reconnect on the hot thread.

## V1 outbound

100 successes, ~10 µs p50, 22 µs p99, pool held through 5 min idle.
Memo-only. Landing-vs-competitor is tomorrow's live race, not RPC poll.

Raw: `arb-cap/swqos_bench/v1.jsonl` `v1.log`
