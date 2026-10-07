# STATE-009 / AUTH-PUBLISH

PAPER predicts from the latest `STATE_COHERENT` generation strictly
preceding trigger N. Not recon-file drain. Not bootstrap leftovers.

```
Yellowstone → STATE-008 stage → coherent pool generation
      → /dev/shm/arb_auth009  (lock-free per-pool ring)
      → PAPER reads predecessor of N
      → apply N → S'
```

Ring depth 16. If N's signature is already in the ring, PAPER uses the
previous entry (YS-before-shred must not apply N twice). Same-slot
entries without a signature match are skipped.

Journalled on every `opp_synced`: `auth_generation`, `auth_slot`,
`auth_tx_sig`, `auth_age_slots`, `state_before_hash`, `predicted_after_hash`.

Acceptance: `auth_age_slots` p50 ≈ 0, p99 0–1 on active flow; then
bit-exact Pump / DLMM shadow. FUNDED stays 0. ARBHOPS0 untouched.
