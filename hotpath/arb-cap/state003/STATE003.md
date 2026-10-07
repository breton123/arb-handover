# STATE-003

Seeing N early is for deciding. Authoritative execution is for canonical S.

```
observe  → S' → opportunity_t     never writes S
confirm  → hot_commit             only after N landed
reject   → discard speculation    S unchanged
refresh  → account snapshot       → SYNCED
```

Only `SYNCED` pools are sendable. Mismatch → `STALE` → send disabled → refresh.

Frozen ±16 kernel is unchanged. Walk distance is measured on a fat control-plane dump.

`paper_orbit` no longer `hot_commit`s shred-seen N.

Until `mwSC5UAu` apply matches chain post-state, no funded sends.
