# Implementation Plan: UAV Stop Deadlock and Predictive Future Margin

## Design

### 1. APP-owned stop ordering

Under `m_containerMutex`, decide whether the stream is already stopped and
perform only the bounded `VideoPublisher::stop*()` state transition. Copy the
response fields, release the lock, then join the object-detection loop and
publish the terminal status. No Core behavior is specialized for UAV.

### 2. Generic scheduler margin

Add a small pure helper for the predictive new-work horizon:

```text
capacity = min(adaptive lookahead, active aggregate window)
reserved = max(1, ceil(capacity / 4))
horizon  = max(1, capacity - reserved)
```

For a one-cursor capacity, the horizon remains one. Retries continue to be
scheduled before new future work and are not charged twice. The 25% reserve is
a generic queueing margin for callback, production, and retry jitter; it never
examines application identity, rate, payload, codec, or sample class.

### 3. Verification

Run focused red/green tests, full `./waf build -j2`, the related native suite,
Python runner tests, linkage and source scans. Prepare a unique frozen result
root only after all gates pass and no other formal writer exists. Execute all
six >=60-second cells once and preserve the terminal aggregate whether PASS or
FAIL.

## Constitution Check

- public API and wire protocol remain stable;
- APP owns UAV thread lifecycle; Core owns only generic scheduling capacity;
- exact-name retry priority and security validation remain unchanged;
- no workload-specific Core branch is permitted;
- MiniNDN is the final verification environment;
- Spec 154 negative evidence remains immutable.

## Rollback

The two source changes are independent: restore the former stop ordering or
the former horizon helper call. A rollback requires a new successor Spec and
new evidence; it never rewrites Spec 155 results.
