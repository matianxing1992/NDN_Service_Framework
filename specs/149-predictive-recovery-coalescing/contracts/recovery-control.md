# Predictive Recovery-Control Contract

For one active subscriber generation:

```text
missing cursors
  -> one shared validated frontier retrieval
  -> cached or one shared validated group-commit retrieval per name
  -> repair Data Interests
  -> bounded Payload retry
  -> explicit terminal gap
```

Invariants:

- at most one frontier Interest is active;
- at most one group-commit Interest per exact name is active;
- every waiter completes once on Data, validation failure, Nack, timeout, or
  stop;
- only validated current-session metadata enters the cache;
- retention pruning and stop bound all cache/waiter state;
- recovery control is not Mapping-first discovery and cannot increment
  `mappingInterests`.

Status fields:

```text
recoveryControlInterests
recoveryFrontierInterests
recoveryGroupInterests
recoveryCoalescedWaiters
recoveryMetadataCacheHits
```
