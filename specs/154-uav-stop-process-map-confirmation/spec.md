# Feature Specification: UAV Stop Process-map Confirmation

**Created**: 2026-07-26  
**Status**: Closed — Negative Baseline (2/6 accepted)  
**Baseline**: Spec 153 decoder repair PASS; formal harness invalid

## Requirements

- **FR-001**: The MiniNDN launcher MUST retain each Drone process handle in a
  map keyed exactly like `drone_logs`.
- **FR-002**: The bounded stop-marker wait MUST use the matching retained
  process and MUST still fail if the marker does not arrive.
- **FR-003**: Static/unit tests MUST prove the process map is defined, populated,
  and used before the formal campaign can start.
- **FR-004**: The validated single-thread decoder and all Spec 152 rate/metric
  contracts MUST remain unchanged.
- **FR-005**: A new complete 10/20/30/40/50/60-fps MiniNDN matrix MUST run once
  in a unique root with no retry or selective replacement.
- **FR-006**: Specs 152/153 and their results remain immutable.

## Success Criteria

- **SC-001**: Full build, 80 related native tests, Python harness tests, syntax,
  linkage, and strict structure gates pass.
- **SC-002**: All 6/6 formal cells pass every unchanged acceptance gate and
  exit normally.
- **SC-003**: Aggregate comparison and post-audit close with stable hashes and
  no Core/workload branch.

## Terminal Result

The one-shot formal campaign is frozen at
`results/spec154-uav-stop-process-map-formal-20260726T081213Z`.
Only 2/6 cells were accepted, so SC-002 was not achieved:

- 10/30/40 fps failed `processExitedNormally` because the UAV stop handler
  could join the object-detection thread while retaining `m_containerMutex`;
- 50 fps failed the unchanged future-hit gate (94.788%) and recorded 1,734
  timeouts despite zero configured loss;
- 20/60 fps passed all gates.

All six rates met the configured-rate, delivery, exact-wire, zero-Mapping,
queue-drain, p99, and longest-gap gates. This is useful negative evidence, but
it is not a 6/6 confirmation and MUST NOT be selectively rerun or overwritten.
