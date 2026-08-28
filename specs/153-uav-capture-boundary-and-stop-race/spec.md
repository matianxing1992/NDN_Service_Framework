# Feature Specification: UAV Low-rate Decode and Stop-race Repair

**Created**: 2026-07-26  
**Status**: Closed — Invalid Harness Campaign  
**Baseline**: Frozen Spec 152 formal FAIL (4/6)

## Requirements

- **FR-001**: File-backed GStreamer capture MUST preserve accurate wall-clock
  pacing and capture-origin-to-encoded-callback lag no greater than
  `max(one frame period, 50 ms)`.
- **FR-002**: The zero-B-frame H.264 decode path MUST not use automatic
  multi-frame thread buffering; it MUST produce a 10-fps median
  capture-origin-to-decoder-output delay at most 200 ms.
- **FR-003**: Focused tests MUST prove requested 10/60 fps, capture callback
  lag, exact frame identity, and the 10-fps decode bound.
- **FR-004**: The MiniNDN launcher MUST wait a bounded five seconds for the
  asynchronous Drone `video stopped` status before failing.
- **FR-005**: No sleep, retry, workload branch, or threshold relaxation may enter
  Core, StreamPublisher, PredictiveStreamSubscriber, wire protocol, FEC,
  Mapping, or prefetch logic.
- **FR-006**: A fresh 10/20/30/40/50/60-fps two-node MiniNDN matrix MUST run
  once using the unchanged Spec 152 measurement/acceptance contract.
- **FR-007**: Spec 152 and all older results are immutable and may not be
  rerun, overwritten, or selectively supplemented.

## Success Criteria

- **SC-001**: Pacing/lag, callback/decode, StreamFacade/UAV, Python harness,
  and full build gates pass.
- **SC-002**: All 6/6 successor formal cells pass rate, delivery, latency,
  future-hit, Mapping, queue, API, exact-wire, and process-exit gates.
- **SC-003**: Aggregate CSV/Markdown reports all metrics and source/binary
  hashes remain stable through execution.

## Out of Scope

- changing thresholds or excluding a rate;
- loss/reorder testing;
- Core/prefetch/FEC/Mapping algorithm changes;
- rerunning Spec 152.

## Terminal Outcome

The decode repair passed focused validation and the first formal 10-fps cell
reduced p50 from 398.483 to 100.868 ms and p99 from 1126.999 to 878.643 ms.
However, the new stop wait referenced an undefined `drone_procs` name, making
the campaign operationally invalid. Execution was stopped after the same
deterministic NameError appeared; the partial root is preserved at
`results/spec153-uav-decode-stop-formal-20260726T080534Z`. Spec 154 owns the
process-map repair and complete successor matrix.
