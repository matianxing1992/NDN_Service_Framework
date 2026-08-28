# Feature Specification: UAV Stop Deadlock and Predictive Future Margin

**Created**: 2026-07-26  
**Status**: Closed — Negative Baseline (2/6 accepted)  
**Baseline**: Frozen Spec 154 negative result (2/6 accepted)

## Problem

Spec 154 proved that `StreamPublisher` and `PredictiveStreamSubscriber` sustain
accurate 10–60-fps UAV video with 99.876–99.923% delivery, zero Mapping
Interests, bounded queues, and sub-second p99 latency. It did not close:

1. the UAV APP stop handler can hold `m_containerMutex`, call
   `stopObjectDetectionLoop()`, and join a thread that is waiting to reacquire
   the same mutex through `isStreaming()`;
2. the predictive scheduler can consume its complete adaptive window with
   future Interests, leaving no margin for production jitter or retries. At
   50 fps this produced 1,734 timeouts in zero loss and a 94.788% future-hit
   ratio.

Spec 154 and its formal root remain immutable negative evidence.

## Functional Requirements

- **FR-001**: The UAV video stop handler MUST stop/join the object-detection
  loop only after releasing `m_containerMutex`.
- **FR-002**: The stop response and `DRONE_STATUS ... video stopped` marker MUST
  be emitted after the stream and detection loop reach their terminal state.
- **FR-003**: A deterministic source/behavior regression MUST fail if a
  potentially blocking join is moved back inside the container lock.
- **FR-004**: Predictive Core MUST reserve a fixed generic safety margin of the
  active aggregate window instead of filling 100% of it with new future
  Interests. Retry work remains higher priority and the resulting horizon MUST
  remain at least one cursor and no greater than lookahead or aggregate window.
- **FR-005**: The safety rule MUST use only generic scheduler state. Core and
  bindings MUST contain no UAV, video, fps, audio, telemetry, codec, sample
  class, or workload-specific branch.
- **FR-006**: Unit tests MUST cover small windows, rounding, lookahead bounds,
  aggregate bounds, and the 25% reserved-margin rule.
- **FR-007**: Public C++/Python API, wire names, FEC format, exact sequential
  payload names, retry cap, topology, and UAV media configuration MUST remain
  unchanged.
- **FR-008**: A fresh MiniNDN runner MUST execute the complete
  10/20/30/40/50/60-fps zero-loss matrix once, in order, with a 5-second
  warm-up and at least 60 seconds measured per cell.
- **FR-009**: Every formal cell MUST preserve the Spec 152 acceptance contract
  and additionally keep retry and timeout counts individually at or below 2%
  of Payload Interests.
- **FR-010**: The runner MUST freeze source/binary/config/command hashes before
  execution, forbid automatic retry and selective replacement, and preserve
  every failed cell.
- **FR-011**: Specs 152–154 and all their results MUST NOT be rerun, modified,
  overwritten, copied into the successor, or presented as passing evidence.

## Success Criteria

- **SC-001**: Focused stop-lock and generic horizon tests pass.
- **SC-002**: Full build, related native tests, Python harness tests, syntax,
  linkage, source-boundary scan, and strict traceability gates pass.
- **SC-003**: All 6/6 fresh formal cells pass rate, delivery, exact-wire,
  decoding, future-hit, latency/gap, queue, retry/timeout, and normal-exit
  gates.
- **SC-004**: One aggregate table reports requested/achieved fps, segment PPS,
  delivery, AoI/end-to-end mean/p50/p95/p99, longest gap, future-hit,
  Mapping/Payload Interest, retry, timeout, Nack, recovery, useless Interest,
  and terminal queues.
- **SC-005**: Post-audit confirms stable hashes, no frozen-result mutation, no
  workload branch, and no unsupported claim beyond zero-loss MiniNDN.

## Out of Scope

- changing the public streaming API or wire protocol;
- changing FEC coding or the bounded retry algorithm;
- changing UAV bitrate, resolution, packetization, or target rates;
- loss/reordering tests, which remain separate frozen evidence;
- physical UAV or camera validation.

## Terminal Result

The immutable formal root is
`results/spec155-uav-stop-deadlock-future-margin-formal-20260726T174654Z`.
All six cells passed rate, delivery, future-hit, latency/gap, exact-wire,
zero-Mapping, bounded-queue, decoding, and normal-exit gates. The APP stop
deadlock is therefore fixed. Only 10/20 fps passed the additional efficiency
gate; 30–60 fps recorded retry/timeout ratios of 2.28%–4.26%, above the fixed
2% limit. The result is frozen at 2/6 and MUST NOT be rerun or selectively
replaced. Spec 156 owns a smaller generic new-work horizon and a wholly new
confirmation.
