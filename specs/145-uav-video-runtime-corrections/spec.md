# Feature Specification: UAV Video Runtime Corrections

**Feature Branch**: `[145-uav-video-runtime-corrections]`  
**Created**: 2026-07-24  
**Status**: Complete  
**Input**: Create a small UAV Video repair Spec that corrects non-30-fps
sample-class consistency, contains exceptional callback boundaries, and shows
the actual Core fetch state. Do not modify or rerun frozen Specs 125/126.
Only after this feature closes may UAV Video become Spec 144's formal reference
implementation.

## Context and Claim Boundary

The current UAV Video path already uses the generic Mapping v2 Streaming API:
the Provider announces future samples, resolves the actual extent, encrypts
opaque source items, and publishes through `publishSample`; Ground Station
opens at `Latest` with `AdaptiveSampleAtomic` and consumes verified items. This
feature does not replace that design.

The code audit found three application-layer defects around that valid Core
path:

1. future class announcement follows configured FPS, while the legacy publish
   fallback classifies with `% 30`, its FFmpeg command fixes the GOP at 60, and
   its byte-grouping path does not preserve exact access-unit boundaries;
2. GStreamer `appsink` C callbacks directly invoke C++ handlers and can allow a
   C++ exception to cross the C ABI;
3. Ground Station receives the actual `StreamFetchDecision` in
   `LiveStreamStatus`, but its displayed `window` and `lookahead` are currently
   recomputed by an application policy function.

Spec 145 corrects only these defects in UAV-APP. It does not change Core
fetching, Mapping, FEC, retry, bindings, or the frozen Spec 125/126 evidence.
It does not optimize the fixed 120 ms announcement lead, change the default
video backend, redesign codec selection, or make a new loss/reorder claim.

Spec 144 remains independent and must not cite UAV Video as its formal
reference implementation until every Spec 145 task and acceptance gate passes.

## User Scenarios & Testing

### User Story 1 - Consistent Future Classes at Supported FPS (Priority: P1)

As a UAV Video operator, I want future Mapping announcements and actual
publication to use one session-frozen key/delta schedule at every supported
frame rate so that a valid non-30-fps stream does not fail from contradictory
application metadata.

**Independent Test**: For 20, 30, and 60 fps, generate multiple GOPs for the
GStreamer exact-access-unit input and multiple legacy byte groups. Verify that
GStreamer key/delta publication equals its immutable announced class and that
legacy announcement/publication consistently uses its bounded conservative
class without pretending that a byte group is a key or delta frame.

**Acceptance Scenarios**:

1. **Given** a supported FPS and a new video session, **when** future samples
   are announced, **then** one immutable class schedule derived from the
   accepted session configuration governs both announcement and publication.
2. **Given** the legacy FFmpeg backend, **when** it produces a byte group
   without exact access-unit identity at 20, 30, or 60 fps, **then** the
   application announces and publishes one conservative bounded `opaque` class
   and does not label the group as a key/delta frame.
3. **Given** the GStreamer backend, **when** an encoded access unit arrives,
   **then** its authoritative `DELTA_UNIT` flag is checked against the already
   announced class and a contradiction fails the current session cleanly.
4. **Given** invalid FPS, an unsupported interval, a skipped sample, or an
   encoder restart, **when** the session cannot preserve its schedule, **then**
   the application rejects or restarts the session at a new decodable boundary
   instead of relabeling committed Mapping.

---

### User Story 2 - No C++ Exception Crosses GStreamer C Callbacks (Priority: P1)

As a UAV Video operator, I want publication and decode callback failures to be
contained so that an exceptional frame stops the affected pipeline with a
stable diagnosis rather than terminating the process through a C ABI.

**Independent Test**: Inject `std::exception` and non-standard exceptions from
capture and decode callbacks. Verify `GST_FLOW_ERROR`, `Failed` state, one
bounded failure reason, no later application callback, and idempotent stop.

**Acceptance Scenarios**:

1. **Given** a capture-side application callback that throws, **when**
   `onCaptureSample` executes, **then** the exception is caught before returning
   to GStreamer, the pipeline becomes `Failed`, and the callback returns a
   GStreamer error.
2. **Given** a decode-side application callback that throws, **when**
   `onDecodeSample` executes, **then** the same containment contract applies.
3. **Given** a failed pipeline, **when** additional frames or repeated stop
   calls arrive, **then** no application callback is invoked again and cleanup
   remains deadlock-free and idempotent.
4. **Given** a failure reason, **when** it is logged or exposed to the owner,
   **then** it is stable, bounded, and contains no video payload or secret.

---

### User Story 3 - Ground Station Shows Actual Core Fetch State (Priority: P1)

As a Ground Station operator, I want the displayed transport window,
lookahead, phase, and capacity reason to come from the active Core consumer so
that the UI does not present an application estimate as Core behavior.

**Independent Test**: Feed known `LiveStreamStatus.fetchDecision` snapshots
through the active stream callback and verify exact display/state equality.
Before the first snapshot, after stop, and after session replacement, verify an
explicit unavailable state rather than estimated transport values.

**Acceptance Scenarios**:

1. **Given** an active consumer and a Core status containing a fetch decision,
   **when** Ground Station publishes `VideoAdaptiveState`, **then** its transport
   window, lookahead, interest lifetime, missing timeout, phase, policy mode,
   capacity reason, and decision reason are copied from that exact decision.
2. **Given** no accepted decision for the active session, **when** the state is
   shown, **then** transport decision availability is false and transport
   values are not silently filled from application policy.
3. **Given** an old consumer callback after stop or replacement, **when** it
   arrives, **then** a session-generation fence prevents it from overwriting
   the current state.
4. **Given** bitrate, decoder backlog, reorder, and aggregate resource caps,
   **when** application adaptation runs, **then** those remain explicitly
   application-owned and are not mislabeled as Core decisions.

## Edge Cases

- FPS is 1 or 60, the currently accepted endpoints.
- FPS changes while a session is active.
- An encoder starts with an IDR and restarts after failure.
- The encoder emits a key/delta flag inconsistent with the frozen schedule.
- A future class has already been committed when the mismatch is detected.
- The publication callback throws before or after protected bytes are created.
- A callback throws a non-`std::exception` value.
- A callback fails while `stop()` is requested from another thread.
- A Core status has no `fetchDecision`.
- A status callback from a retired stream arrives after a replacement starts.
- The application bitrate estimator changes while the actual Core window does
  not.

## Requirements

### Functional Requirements

- **FR-001**: The feature MUST preserve
  `specs/125-adaptive-sample-atomic-prefetch`,
  `specs/126-loss-reorder-resilience`,
  `results/spec125-adaptive-sample-atomic-20260719-confirm06`, and
  `results/spec126-loss-reorder-20260720-confirmation07` byte-for-byte and MUST
  verify their before/after directory-root digests without invoking either
  historical runner.
- **FR-002**: The change MUST remain in UAV-APP and focused tests/tooling.
  `ndn-service-framework/Stream.*`, ServiceUser/Provider, Python bindings, and
  generic retry/FEC/Mapping logic MUST remain unchanged unless a new audit
  proves an unavoidable generic defect; such a finding blocks this small Spec
  and requires explicit scope approval. The current on-disk UAV files contain
  pre-existing user changes and are the implementation baseline; Spec 145 MUST
  preserve all unrelated hunks and MUST NOT reset, normalize, or overwrite
  them.
- **FR-003**: Each video session MUST freeze one validated FPS and
  backend-specific class mode before future announcement begins. The
  GStreamer exact-access-unit mode MUST freeze a key-frame interval used by
  announcement, encoder configuration, and actual-class validation.
- **FR-004**: The implementation MUST remove hard-coded `% 30` publication
  classification. Because the legacy byte-group path lacks exact access-unit
  identity, it MUST use one conservative `opaque` sample class for both
  announcement and publication, with a declared hard source-extent bound at
  least as large as every supported legacy group; it MUST NOT synthesize
  key/delta truth from frame sequence. Its fixed GOP setting MAY be derived
  from accepted FPS for encoder behavior but MUST NOT be treated as proof of
  group class.
- **FR-005**: Already committed future Mapping MUST NOT be silently relabeled.
  An actual/announced class contradiction MUST fail the affected session with
  a stable reason.
- **FR-006**: Both GStreamer capture and decode C entry points MUST be
  `noexcept` in effect: all C++ exceptions MUST be caught before returning to
  GStreamer.
- **FR-007**: A callback failure MUST atomically record the first bounded
  failure reason, transition the pipeline to `Failed`, suppress later
  application callbacks, and return `GST_FLOW_ERROR` or the documented
  equivalent.
- **FR-008**: Callback failure handling and stop/destruction MUST be
  deadlock-free and idempotent and MUST NOT call blocking pipeline teardown
  recursively from the streaming callback.
- **FR-009**: Ground Station MUST cache only the most recent actual
  `StreamFetchDecision` delivered for the active consumer generation and clear
  it on stop/replacement.
- **FR-010**: `VideoAdaptiveState` MUST explicitly report whether a Core fetch
  decision is available and identify its source. Existing transport fields
  shown as Core state MUST equal the actual decision when available and be
  unavailable/zero with an explicit reason otherwise.
- **FR-011**: Core-owned fields MUST include at least window, lookahead,
  interest lifetime, missing timeout, phase, policy mode, capacity reason, and
  decision reason. APP-owned bitrate advice, pressure interpretation, decoder
  backlog, reorder limits, and configured resource caps MUST remain separately
  labeled.
- **FR-012**: A generation/session fence MUST reject status callbacks from a
  retired live-stream handle.
- **FR-013**: Deterministic tests MUST cover 20, 30, and 60 fps, legacy
  conservative-class and GStreamer exact-class inputs, encoder restart and
  actual-class mismatch, both callback directions, standard and non-standard
  exceptions, unavailable/active/stale Core status, and wire/field round trips
  for any additive `VideoAdaptiveState` fields.
- **FR-014**: Existing native UAV protocol/streaming tests, Python unified
  video tests, and the UAV stream security contract MUST remain passing.
- **FR-015**: One fresh Spec 145 MiniNDN acceptance cell MUST exercise the
  GStreamer reference path at 20 fps for a 60-second measured window on the
  normal two-node zero-loss topology. It MUST use a new result directory and
  MUST NOT invoke, overwrite, or copy a Spec 125/126 result.
- **FR-016**: The fresh cell MUST report frame attempts/publications/delivery,
  class mismatches, pipeline failures, actual Core decision availability,
  provider future Interests/hits, Mapping/Payload Interests, retry, timeout,
  Nack, duplicate delivery, and end-to-end mean/p50/p95/p99.
- **FR-017**: The acceptance cell MUST have zero class mismatch, zero uncaught
  callback exception/process crash, active Core status sourced from the
  callback, all 12 five-second buckets active, no duplicate application
  delivery, provider future-hit ratio at least 99%, payload Interest overhead
  at most 25%, p95 at most 300 ms, and p99 at most 600 ms.
- **FR-018**: A failed fresh Spec 145 acceptance cell MUST be preserved. It may
  be diagnosed in a separately named directory but MUST NOT be represented as
  passing through selective replacement.
- **FR-019**: Spec 144 MUST remain unchanged during Spec 145 implementation.
  Only after all Spec 145 requirements, tests, evidence, post-implementation
  audit, and frozen-hash checks pass MAY a closure task update Spec 144 to cite
  the corrected UAV Video path as its formal reference implementation.
- **FR-020**: The final audit MUST prove that the repair introduced no manual
  fetch loop, no duplicate prefetch policy, no new Core special case, and no
  change to frozen Spec 125/126 files or results.

## Success Criteria

- **SC-001**: The deterministic 20/30/60-fps matrix records zero discrepancy
  between announced and published class for every generated sample, with
  GStreamer using exact key/delta classes and legacy using only its bounded
  conservative class.
- **SC-002**: Every injected capture/decode callback exception is contained;
  the process survives, state becomes `Failed`, no later callback fires, and
  repeated stop succeeds.
- **SC-003**: Every displayed Core transport field exactly matches the injected
  active-session `StreamFetchDecision`; unavailable and stale-session cases
  never fall back to APP estimates.
- **SC-004**: All focused regression/security gates pass and the four frozen
  directory-root digests equal their recorded baseline values.
- **SC-005**: The single fresh 20-fps MiniNDN cell meets FR-017 without
  modifying its preregistered command or thresholds after execution.
- **SC-006**: Post-implementation CodeGraph/source audit reports zero changed
  generic Core/binding symbol and zero new UAV-side transport algorithm.

## Out of Scope

- Rerunning, editing, reinterpreting, or selectively supplementing Spec 125 or
  Spec 126.
- Changing Spec 125/126 acceptance thresholds or claims.
- Optimizing the 120 ms future-announcement lead or Mapping block efficiency.
- Changing adaptive fetch, retry, timeout, Nack, FEC, or recovery algorithms.
- Changing the default UAV video backend.
- Codec quality, visual fidelity, microphone/acoustic work, or physical UAV
  hardware validation.
- Starting the Spec 144 formal matrix.
