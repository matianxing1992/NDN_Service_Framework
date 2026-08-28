# Implementation Plan: Predictive Bounded Catch-up

## Baseline

Use the immutable Spec 150 build-Core smoke:

```text
/tmp/spec150-build-core-smoke.dA5IqJ
```

It proves ordered-drain health but exposes a catch-up deficit:

```text
provider pushed 41866
consumer Payload Interests 15142
delivered 13280
next cursor 13390
ready/gap queues 0/0
```

## Design

Keep the adaptive controller authoritative:

```text
effectiveFutureHorizon =
  max(1, min(decision.lookahead, aggregate in-flight limit))

latestKnownProduced + effectiveFutureHorizon
  -> maximum schedulable cursor
```

The horizon is an address-space bound, not additional capacity. The existing
aggregate budget and pre-reservation set still bound concurrently expressed
Interests. Retry-pending cursors are scheduled before new future cursors.

Expose `futureCursorHorizon` so tests and MiniNDN evidence can distinguish the
one-sample demand estimate from the permitted bounded catch-up horizon.

## Validation Order

1. deterministic horizon/budget/retry-priority tests;
2. focused native and Python parity tests;
3. build-Core impaired MiniNDN/UAV smoke;
4. full build, all native tests, Python tests, linkage and workload scans;
5. independent runner/analyzer preflight and immutable hash freeze;
6. exactly one two-cell formal campaign;
7. post-implementation audit.

Formal topology, workload, warm-up, measured window, impairment, and reporting
remain byte-for-byte equivalent to Spec 150 except for the new runner/analyzer
identity and source hashes.
