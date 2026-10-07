Output of `tools/anchor/snapshot.py`. One coherent Pump vector per
line from a **finalized** `getMultipleAccounts`. `slot` is that
response's `context.slot`. Arm CompactState, then replay OF `slot > S`.
The OF buffer must retain enough history to span finalization lag.
