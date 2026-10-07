# arb-exec

Turn an `opportunity_t` into signed transaction bytes ready to send.

```text
arb-core    opportunity_t
arb-exec    signed tx bytes
```

This tree does not quote, size, or predict. It stops at `leader_send()`. No OrbitFlare, no DoubleZero, no sockets beyond the local UDP stub.

```text
EXEC-001            route-0 executor + local proof     FROZEN  account layout
EXEC-002            unsigned tx template + patch       FROZEN  offsets
EXEC-003            Ed25519 sign of frozen message     FROZEN  libsodium
EXEC-004            durable nonce pool                 FROZEN  64-slot ring
EXEC-005            leader_send stub                   FROZEN  local UDP
EXEC-005A           funded mainnet smoke               memo + durable nonce
```

## EXEC-001

Smallest real transaction for route 0:

```text
our wallet
   ↓
OUR_EXEC
   ↓
DLMM swap
   ↓
Pump swap
   ↓
final balance
   ↓
profit >= min_profit
      ├─ yes → success
      └─ no  → revert
```

Both directions. `amount_in` and `direction` come from `opportunity_t`. Output returns to our quote inventory. A stale or unprofitable second leg reverts the first. Account order is deterministic. Local CU is recorded.

Live DLMM / Pump replace the shims. They do not reorder accounts.

```bash
cd ~/arb-exec
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/exec001
```

EXEC-001 is frozen. Do not change `route0_process`, `route0_pack`, or the account table in `route0.h`.

## EXEC-002

Offline: bake the 35-account vector, OUR_EXEC, ComputeBudget, CU-limit ix, and route0 skeleton into `route0_tx_template`. Pump base/quote mints alias DLMM X/Y. No ALT.

Hot path: memcpy the template, then patch five constant offsets — `amount_in`, `direction`, `min_profit`, blockhash/nonce, CU price. No heap, no metas, no pubkey encode.

```bash
cd ~/arb-exec
cmake --build build
./build/exec002 --cpu 47 --mlock
```

EXEC-002 is frozen. Do not change `route0_tx_compile`, `route0_tx_patch`, or the offset constants.

## EXEC-003

Solana signs the message, not the tx. Offsets are fixed: signature at 1, message at 65, length 1240.

Signer is prepared once (libsodium 1.0.18, seed→expanded sk, mlock). Hot path is `crypto_sign_ed25519_detached` into the signature slot. OpenSSL 3 verifies independently. No keyfile or heap on the sign path. Test keys are getrandom, never committed.

```bash
cd ~/arb-exec
cmake --build build
./build/exec003 --cpu 47 --mlock
```

libsodium wins (~15 µs p50). OpenSSL is ~2× on the same 1240-byte message. Same-message vs patched is the same cost. `T(opportunity → signed tx) ≈ T(Ed25519)`.

EXEC-003 is frozen. Do not change `route0_sign` or the message/signature offsets.

## EXEC-004

64-slot durable nonce ring. One exec thread, no locks. Control plane loads hashes and reloads after IN_FLIGHT → REFRESH. Hot path is `nonce_claim` → READY hash. Patch that hash into the frozen blockhash slot. No RPC on the claim path. Not 992.

```bash
cd ~/arb-exec
cmake --build build
./build/exec004 --cpu 47 --mlock
```

Claim is 84 cycles (20 ns). Noise next to the signature. EXEC-004 is frozen. Do not change `nonce_claim` or grow the pool to 992.

## EXEC-005

`leader_conn_t leaders[LEADER_N]` and `leader_send(conn, tx, len)`. Connected UDP, opened on the control plane. Local loopback only. Do not turn this into a public TPU client. DoubleZero replaces it.

```text
opportunity → nonce → patch → sign → leader_send()
```

```bash
cd ~/arb-exec
cmake --build build
./build/exec005
```

EXEC-005 is frozen. Do not grow `leader_send`.

## EXEC-005A

Funded mainnet smoke. Harmless memo, then one durable-nonce memo. Our bytes, our signer. RPC `sendTransaction` lands them (mainnet TPU is QUIC; UDP TPU is gone). Not the arb. Not live DLMM/Pump.

Keys and RPC URL come from `SMOKE_ENV` / `~/.arb-smoke.env`. Never commit them.

```bash
cd ~/arb-exec
cmake --build build
./build/exec005a
```

Landed on mainnet (our bytes, our signer):

```text
A  memo          5wMEJxcS...G45GKn   slot 449845732
   create-nonce  2jgTCsSr...g6PRc    slot 449845736
B  nonce-memo    2mqYqNQj...P7gNw    slot 449845887
```


## Layout

```text
include/     opportunity  route0  tx_template  sign  nonce  leader
src/         route0.c  tx_template.c  sign.c  nonce.c  leader.c
tests/       exec001–exec005
```



