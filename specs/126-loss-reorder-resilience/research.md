# Research: Loss and Reordering Resilience

## Decision 1: Reuse Mapping v2 and exact-name retries

**Decision**: Keep Spec 125 Mapping v2 and retry the same semantic Data name.

**Rationale**: Mapping already authenticates cursor, group, class, item index,
predicted extent, and real semantic name. Loss and arrival order do not alter
those facts. A wire revision would add migration and interop risk without
preventing a demonstrated failure.

**Alternatives considered**: Mapping v3 attempt IDs and new recovery names were
rejected because attempt identity is local transport state and recovery must
not create a second application object.

## Decision 2: Make cursor lifecycle authoritative

**Decision**: Represent attempt ownership and terminal disposition as one
bounded cursor state machine; keep existing sets only as indexes.

**Rationale**: Current code distributes ownership across in-flight,
processing, completed, attempt-count, and expressed-time maps. Deterministic
timeout/Data/validation permutations need one place to prove at-most-once
terminal behavior and stop-generation fencing.

**Alternatives considered**: Adding more membership checks was rejected because
it multiplies pairwise invariants and leaves callback-attempt identity implicit.

## Decision 3: Keep FEC recovery group-scoped

**Decision**: Reuse authenticated one-repair XOR recovery; accept any arrival
order, recover exactly one missing source, and fail closed for larger loss.

**Rationale**: The existing repair envelope already binds source names,
cursors, lengths, group, and repair name. Stronger codes or multiple parity
items are outside the measured 1% boundary and would change APP policy.

**Alternatives considered**: Reed-Solomon, additional parity, and adaptive FEC
were rejected as unrequested mechanisms and confounders.

## Decision 4: Reordering belongs at two existing layers

**Decision**: Core accepts independently validated Data in callback order; UAV
uses the existing media-sequence reorder buffer for decoder order.

**Rationale**: Core is codec-neutral and cannot define frame order. UAV already
maps publication cursors to source-only media sequences and owns decoder gaps.

**Alternatives considered**: Serializing Core validation or parsing H.264 in
Core was rejected because it reduces concurrency and violates ownership.

## Decision 5: Use matched bidirectional netem profiles

**Decision**: Apply the same captured `tc netem` profile on both link endpoints.
Use 1% iid loss and a bounded reorder profile of 20 ms delay, 10 ms jitter,
25% reorder, 50% correlation, gap 5.

**Rationale**: NDN Interests and Data travel in opposite directions; impairing
one direction alone makes attribution ambiguous. Reordering needs nonzero delay
to be observable. The profile is large enough to exercise callback order but
well below existing live deadlines.

**Alternatives considered**: 5% loss is deferred because prior UAV work shows
it crosses a broader reliability boundary; starting there would conflate this
state-machine fix with FEC-policy research. Unseeded one-off runs were rejected
in favor of five transparent repetitions.

## Decision 6: Engineering boundary, not hypothesis victory

**Decision**: Use 1 zero-loss regression and 5 repetitions for each impaired
profile, report every result and exact binomial intervals, and make no general
population claim.

**Rationale**: Netem has no usable stable seed in this environment. Five runs
can enforce a clear 4/5 acceptance boundary and reveal gross instability but
does not provide high statistical power. More runs would consume time without
changing the initial implementation decision.

**Alternatives considered**: A single favorable cell and automatic reruns were
rejected as evidence selection. A large powered study is deferred until the
algorithm is stable and a research hypothesis is specified.

## Decision 7: Preserve negative and historical evidence

**Decision**: Hash Spec 125 evidence before/after, invoke each frozen Spec 126
command exactly once, and require a full new confirmation series after source
changes.

**Rationale**: Selective replacement destroys matched comparison and masks the
boundary the feature is intended to discover.

**Alternatives considered**: Retrying failed repetitions and merging post-fix
runs into the first matrix were rejected.
