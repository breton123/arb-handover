# Offline LUT cache

Not a hot-path RPC client.

One jsonl line per table:

```text
{"key_hex":"<32-byte pubkey hex>","addrs_hex":["<32-byte hex>",...]}
```

Load before replay / next capture. Do not hand-grow this file for live
coverage. Live uses a sparse cache plus off-path `getAccountInfo`
(`docs/wire/LUT-001.md`). Empty cache ⇒ v0 lookups stay fail-closed.
