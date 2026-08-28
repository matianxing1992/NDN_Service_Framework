# Feature Specification: Predictive UAV Multi-rate Validation

**Created**: 2026-07-26  
**Status**: Closed — Negative 4/6 Baseline  
**Baseline**: Frozen Specs 148–151; no historical campaign may be rerun

## Problem

The current UAV Video application uses `StreamPublisher::start/push/flush` and
`PredictiveStreamSubscriber`, and the frozen 30-fps Spec 151 campaign proves
wire identity, delivery, prefetch, recovery, and ordered drain. It does not
prove that the configured application frame rate is the actual wall-clock
production rate or that the same API remains stable across rates.

Source and log inspection found that the file-backed GStreamer capture pipeline
uses `appsink sync=false`. `videorate` assigns the requested timestamps, but the
non-live file source can consequently run faster than wall clock. The frozen
30-fps run decoded about 50 frames/s. Historical evidence remains valid for its
original transport claims, but cannot be reused as multi-rate pacing evidence.

## Functional Requirements

- **FR-001**: The UAV GStreamer file-backed capture path MUST pace output
  against the pipeline clock so configured FPS controls wall-clock frame
  production.
- **FR-002**: Live-source behavior, exact frame identity, key/delta class
  consistency, `StreamPublisher::start/push/flush`, and
  `PredictiveStreamSubscriber` MUST remain unchanged.
- **FR-003**: Deterministic/focused tests MUST verify capture pacing without
  weakening callback containment or decode behavior.
- **FR-004**: A new MiniNDN runner MUST execute exactly six zero-loss,
  two-node UAV cells at 10, 20, 30, 40, 50, and 60 fps, in that order.
- **FR-005**: Every formal cell MUST use 5 seconds of warm-up and at least 60
  seconds of measurement, the same topology, 8 Mbps bitrate, 480-pixel width,
  one FEC repair item, predictive API, binaries, and Core configuration.
- **FR-006**: The runner MUST freeze source/binary/config/command hashes before
  execution, forbid retry/rerun, preserve failed cells, and use a unique result
  root.
- **FR-007**: Each cell MUST report requested and achieved frame rate,
  frame-rate error, pushed/admitted/delivered counts and rates, delivery ratio,
  AoI/end-to-end mean/p50/p95/p99, longest gap, future-hit ratio,
  Mapping/Payload Interests, retry, timeout, Nack, recovery ratio, useless
  Interest ratio, and terminal queue state.
- **FR-008**: Every cell MUST achieve requested FPS within ±5%, delivery at
  least 98%, future-hit at least 95%, zero Mapping Interests, zero persistent
  ready/gap queue, exact wire identity, valid decoding, and complete latency
  distributions. End-to-end p99 and longest delivery gap MUST each be at most
  1000 ms in this zero-loss matrix.
- **FR-009**: `StreamPublisher` and `PredictiveStreamSubscriber` MUST be active
  in the same stream session in every cell; no old Mapping-first path or manual
  fetch loop is permitted.
- **FR-010**: Generic Core and bindings MUST contain no UAV, video, FPS, codec,
  or workload-specific branch. A pacing correction belongs only to UAV-APP.
- **FR-011**: Specs 148–151 and their results MUST remain immutable and MUST
  NOT be rerun, overwritten, or selectively copied.
- **FR-012**: If a pre-formal diagnostic exposes a defect, preserve that
  diagnostic, repair only the generic or UAV-owned root cause, rebuild, and use
  a new diagnostic root. Once the formal matrix starts, no repair or selective
  rerun is allowed; a formal failure is frozen negative evidence.

## Success Criteria

- **SC-001**: Focused pacing tests pass and demonstrate real-time output at
  representative low/high rates.
- **SC-002**: A short MiniNDN preflight proves the measured-rate parser,
  build-Core linkage, API markers, and rate error before the formal freeze.
- **SC-003**: All 6/6 formal cells pass every FR-008 gate.
- **SC-004**: One aggregate CSV/Markdown table compares all required metrics
  across rates and records immutable input/output hashes.
- **SC-005**: Source audit finds no workload branch in Core and no changed
  frozen Spec/result.

## Out of Scope

- packet loss or reordering (already covered by frozen Spec 151);
- a new prefetch, retry, FEC, Mapping, or wire algorithm;
- changing high-level API signatures;
- physical UAV/camera validation;
- rerunning or changing Specs 148–151.

## Terminal Outcome

The immutable six-cell campaign at
`results/spec152-predictive-uav-rate-formal-20260726T074501Z` completed 4/6
PASS. All rates and delivery gates passed, but 10 fps failed the p99 latency
gate and 10/30 fps exposed an asynchronous stop-marker race in the MiniNDN
launcher. Spec 152 is frozen and will not be rerun. Spec 153 owns both repairs
and a complete successor matrix.
