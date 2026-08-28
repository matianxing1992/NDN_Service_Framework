# Research: Stream Prefetch Retention Recovery

## Decision 1: Treat the observed stop as a lifecycle bug, not a window-tuning problem

**Rationale**: In the preserved run, the Provider continued to cursor 23,412 while the consumer stopped at 8,753. The Provider later held 129 Interests for names that had already fallen outside its 600-item retention. Increasing the consumer window would increase, not remove, this stale work.

**Alternatives considered**: Increase only `aggregateInterestLimit`; rejected because the current scheduler already filled that limit. Increase only retention; rejected as mitigation without correct evicted/future classification.

## Decision 2: Preserve immutable semantic Data names

**Rationale**: Returning a different "evicted" payload under the original Data name would violate immutable name-to-content expectations and could conflict with cached signed Data. Recovery therefore uses bounded timeout/skip/live-edge behavior rather than alternate content under the same name.

**Alternatives considered**: Signed tombstone under the original payload name; rejected. New status wire protocol; deferred because the existing bounded scheduler can recover without expanding the protocol.

## Decision 3: Use the adaptive decision already present

**Rationale**: `StreamAdaptiveFetcherState::decide()` already computes normalized packet demand, bounded Interest lifetime, current aggregate limit, and payload/Mapping budgets. The consumer currently ignores several of those outputs and recomputes a lifetime from stale descriptor state.

**Alternatives considered**: Add a second UAV-specific fetch controller; rejected because it duplicates Core policy and breaks ownership.

## Decision 4: Size UAV retention by duration

**Rationale**: The fixed 600-item retention is only about 1.54 seconds at 13 items/frame and 30 FPS. A named bounded duration makes the operational meaning explicit while remaining memory-bounded.

**Alternatives considered**: Unbounded retention; rejected. Depend on Repo for every live miss; rejected because it changes latency and live ownership.
