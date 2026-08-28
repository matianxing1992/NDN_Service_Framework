# Ordered-Drain Contract

For one active generation:

```text
nextDeliverCursor
  + terminal gap at cursor -> skip exactly once
  + ready item at cursor   -> callback exactly once
  + neither                -> sleep with no lost wake
```

Invariants:

- one drain owner;
- every ready/gap mutation requests a drain under the same mutex;
- cursor advances monotonically;
- callbacks execute outside the state mutex;
- data behind the cursor is late, never ready;
- stop fences all pending owners/wakes by generation.
- cursors selected by a scheduler are reserved against the aggregate in-flight
  budget before the state mutex is released.
- bounded source retries share that budget and are selected before new future
  cursors.
- MiniNDN processes resolve `libndn-service-framework.so.0.1.0` from the
  campaign-frozen `build/` directory, never the stale `/usr/local/lib` copy.

Recovery lookup:

```text
PredictiveStreamFrontier contractVersion = 2
signed group reference = groupName + firstCursor + lastCursor
missing cursor -> exactly one containing range -> one group fetch
```

Version 1 frontiers do not carry authenticated cursor ranges and are rejected
by the version-2 decoder/validator rather than silently falling back to a
linear group scan.

Required status:

```text
nextDeliverCursor
readyQueueDepth
oldestReadyCursor (optional)
terminalGapQueueDepth
drainWakeCount
staleReadyDrops
```
