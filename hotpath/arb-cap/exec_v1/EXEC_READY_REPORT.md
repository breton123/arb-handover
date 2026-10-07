# Execution readiness

Read-only. No SOL was moved, no ATA or lookup table was created, no plane was published, and nothing was sent.

Route0 templates audited: **3**. Report `RACE_READY` (template + alt + vector + ata + unsigned sim): **3**.

The plane's uppercase `RACE_READY` means both directions compiled and the ATAs existed when the plane was written. This report does not treat that flag as a Custom(6) proof.

## Coverage

| check | pass |
|---|---|
| TEMPLATE_VALID | 3 / 3 |
| ALT_VALID | 3 / 3 |
| VECTOR_VALID | 3 / 3 |
| ATA_VALID | 3 / 3 |
| SIM_VALID | 3 / 3 |
| RACE_READY | 3 / 3 |

## Failure reasons

- `ok` 3

## CU, size, and the fee the wire bills

Every template requests CU limit 400000. Oneshot patches CU price 800000. The fee is signature 5000 + limit*price/1e6 + SWQOS 150000 + safety 50000 = **525000** lamports on every route0 send. Consumed CU from simulation does not change that bill. The hurdle was not lowered to consumed CU, because the template still requests 400000.

| dir | n | cu min | cu p50 | cu max | raw |
|---|---|---|---|---|---|
| 0 | 3 | 116976 | 121881 | 127997 | 623 |
| 1 | 3 | 130955 | 135754 | 157040 | 626 |

## Programs

- OUR_EXEC `38dsYLgtLkHKDMYNBemkEM3WkvZSUo3RvxZSm7eMDj4K` executable=True upgradeable=True ARBEXEC0_in_programdata=False reason=`ok`
- ARBHOPS0 id `CnddPhKV1fnKE7ic5nSJcoVq2XFmQ9daE3tuqc2u6qTt` executable=True upgradeable=True disc_in_programdata=False so_has_disc=False so_bytes=17136 reason=`ok`

## Gate bypasses

- `FUNDED` defaults to 0 and returns before the signer, the racer, and RPC.
- STATE-008 READY is the only path that can arm. `AUTH_READY` pointing at state007 or any other file is refused.
- Journal `race_ready` (`tx_exact`) and plane `RACE_READY` are both required. They are not OR'd.
- The send floor is the wired 525000, not the paper per-sequence hurdle.
- Family 6 (`dlmm-dlmm`) and any seq other than `dlmm-pump` / `pump-dlmm` are `not_v1_executable`. A shared pubkey no longer looks like a route0 send.

## Remaining blockers before one funded shot

- Live AUTH READY absent (/home/louis/captures/state008/READY). Generation cap=/home/louis/captures/state008. This is the current soak, not the oneshot arm path.
- Oneshot arm READY absent (/home/louis/captures/state008/READY). oneshot_live still refuses until this exact path exists.
- paper_orbit was not restarted by this audit. A live hour still has to be searched after STATE is ready.

## One shot

All of these must be true, and a human must say so in that request:

1. `prearm_check.py` exits 0.
2. `/home/louis/captures/state008/READY` exists because the AUTH writer is coherent. state007 READY does not count.
3. At least one `dlmm-pump` or `pump-dlmm` route in this report is `RACE_READY`.
4. `FUNDED=1` for one `oneshot_live.py` process, then back to 0. No retry.

