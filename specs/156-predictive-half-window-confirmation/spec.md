# Feature Specification: Predictive Half-window Confirmation

**Created**: 2026-07-26  
**Status**: Closed — 6/6 PASS  
**Baseline**: Frozen Spec 155 negative result (2/6)

## Requirements

- **FR-001**: New speculative predictive Interests MUST use at most half of
  `min(lookahead, active aggregate window)`, with a minimum of one cursor for a
  nonzero capacity.
- **FR-002**: Retry scheduling remains higher priority and is not reduced by
  the new-work horizon.
- **FR-003**: The rule MUST depend only on generic scheduler capacity and MUST
  contain no application, payload, rate, codec, sample-class, or workload
  branch.
- **FR-004**: UAV stop ordering, public C++/Python API, wire protocol, security,
  FEC, retry cap, topology, media settings, and all Spec 155 acceptance gates
  remain unchanged.
- **FR-005**: Unit/source tests MUST prove half-window rounding and the
  workload-neutral boundary.
- **FR-006**: After all build/test/linkage/audit gates pass, one fresh complete
  10/20/30/40/50/60-fps MiniNDN matrix MUST run with no retry or selective
  replacement.
- **FR-007**: Specs 152–155 and their result roots remain immutable.

## Success Criteria

- **SC-001**: Focused and related regressions plus full build pass.
- **SC-002**: All 6/6 formal cells pass the unchanged Spec 155 matrix contract,
  including retry and timeout ratios individually <=2% of Payload Interests.
- **SC-003**: Aggregate evidence contains every required rate, delivery,
  latency, Interest, retry/timeout/Nack, recovery, useless-Interest, queue, and
  lifecycle metric with stable hashes.

## Out of Scope

New APIs, wire/FEC/retry algorithms, workload tuning, loss/reorder, and changes
to frozen evidence.

## Terminal Result

The immutable campaign at
`results/spec156-predictive-half-window-formal-20260726T181835Z` passed 6/6.
Across 10–60 fps, delivery was 99.885–99.923%, future-hit was
99.703–99.823%, p99 end-to-end latency was 55.7–129.0 ms, retry/Payload was
0–0.196%, and timeout/Payload was 0–0.196%. Every cell had zero Mapping
Interests, zero Nacks, empty terminal queues, exact wire identity, valid
decoding, and normal APP/process shutdown.
