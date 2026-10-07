# RS-FAST-001

Source: `bench/rs/main.c` (`make` target `rs_bench`).

Layout: 32 data + 32 coding, 987-byte shards (Solana data payload).

Report p50/p99 for missing = 1, 2, 4, 8, 16, 32.

Frankfurt `195.242.152.178` (`-O3 -march=native`), 400 samples after 20 warm, 2026-09-27.

| missing | p50 ns | p99 ns |
|--------:|-------:|-------:|
| 1 | 1081 | 1092 |
| 2 | 2273 | 2293 |
| 4 | 4537 | 8382 |
| 8 | 8412 | 14672 |
| 16 | 16855 | 27521 |
| 32 | 33810 | 62093 |

Live cap recover mean (mixed erasure patterns, includes cold cache): 91.9 µs/attempt, down from 2.23 ms.

WSL/laptop the same day (not the scoreboard): miss=1 p50 2.2 µs.
