# arb-feed

Inbound source layer. The only thing that leaves this tree toward the searcher is the frozen `rx_packet_t`.

```text
UDP / later AF_XDP
        ↓
   rx_packet_t          data + len + queue
        ↓
   arb-core
```

OrbitFlare and DoubleZero are sources. They do not appear in arb-core or arb-exec.

Timestamps live on the capture record, not on `rx_packet_t`. Downstream does not change when the source does.

## Capture

Off-path, lossy SPSC. The trade loop never waits on disk.

```text
packet ──→ observe / core
   │
   │ try_push
   ▼
lossy ring
   │
   ▼
recorder thread
   │
   ▼
feed-YYYYMMDD-HHMMSS.cap
```

Record is the whole datagram:

```text
rx_ns  rx_tsc  len  seq  data[]
```

File header stores realtime0 / mono0 / tsc_hz so wall-clock join is possible later.

## Trial (do not start the hour until dry runs pass)

```bash
cd ~/arb-feed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/feed001
./build/feed_load
mkdir -p /tmp/feedcap
./build/feed_live --bind 127.0.0.1 --port 39902 --out /tmp/feedcap \
    --prefix dry --min-gb 1 --cpu 47 --rec-cpu 46 --mlock
# other terminal: send UDP, then SIGINT
./build/feed_replay --file /tmp/feedcap/dry-*.cap
```

Live trial (only after unattended dry run):

```bash
mkdir -p ~/captures
./build/feed_live --bind 0.0.0.0 --port PORT --out ~/captures \
    --prefix orbitflare --rotate-bytes 2147483648 --min-gb 40 \
    --cpu 47 --rec-cpu 46 --mlock
```

Leave it alone. SIGINT flushes the ring.

Observation is atomics + a 1 Hz snapshot. No per-packet stdout. Pool hits and `opportunity_t` are not in this tree.

## Layout

```text
include/   rx (frozen)  shred  classify  feed
src/       udp_source  recorder  observe  live  replay  cap001
tests/     feed001  feed_load
```

## Offline (CAP-001…)

Raw `~/captures/shredstream-*.cap` are the trial corpus. Do not delete, rewrite, or rotate them. Analysis is read-only; derived files go in `~/captures/derived/`.

```bash
./build/cap001 --dir ~/captures > ~/captures/derived/cap001.txt
```
