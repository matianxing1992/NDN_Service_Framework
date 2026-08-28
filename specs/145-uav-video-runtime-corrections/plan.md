# Implementation Plan: UAV Video Runtime Corrections

**Branch**: `[145-uav-video-runtime-corrections]` | **Date**: 2026-07-24  
**Spec**: [spec.md](spec.md)

## Summary

Correct three UAV Video application defects without changing the generic
Streaming Core: freeze one per-session GStreamer class schedule and give the
non-frame-exact legacy backend a truthful bounded conservative class; contain
all exceptions at the two GStreamer C callbacks; and cache/display the actual active-consumer
`LiveStreamStatus.fetchDecision` rather than recomputing Core-looking fields in
UAV-APP.

Implementation remains small and test-first. After deterministic and focused
regression gates pass, run one new 60-second, 20-fps, zero-loss MiniNDN cell.
Specs 125/126 and their canonical results remain immutable and are hash-checked
only. Spec 144 is updated to name this path only after Spec 145 closes PASS.

## Technical Context

**Language/Version**: C++17 and Python 3 experiment tooling  
**Dependencies**: ndn-cxx, NDNSF Mapping v2 Streaming, GStreamer appsink,
FFmpeg/libx264, Boost.Test, MiniNDN  
**Storage**: in-memory per-session class schedule, pipeline failure state, and
latest active-session fetch-decision snapshot  
**Testing**: focused Boost.Test, Python UAV video/security regressions, one
fresh MiniNDN acceptance cell  
**Target Platform**: Linux; GStreamer-enabled UAV applications in two-node
MiniNDN  
**Constraints**: UAV-APP-only implementation; no historical runner; no frozen
result write; no generic Core/binding change; one fresh formal cell only  
**Performance Goal**: preserve the existing Spec 125 zero-loss envelope while
adding correct 20-fps behavior and truthful status  
**Scale/Scope**: three behavior corrections, one application path, one
fresh 60-second acceptance cell

## Constitution Check

- **Dynamic runtime and unified stream API: PASS.** Existing Mapping v2
  `announceSample`/`prepareSampleExtent`/`publishSample` and
  `openLiveStream` remain the only transport path.
- **Security: PASS.** No protected payload or name-binding behavior changes.
- **CodeGraph first: PASS.** Announcement/publication, GStreamer callbacks,
  `VideoAdaptiveState`, and status callbacks were traced before design.
- **Spec-driven change: PASS.** This document owns the repair boundary and
  frozen evidence contract.
- **MiniNDN final validation: PASS by design.** One new cell is allowed; host
  NFD is not final evidence.
- **Frozen evidence: PASS by design.** Historical runners are prohibited and
  before/after digests control closure.
- **GSD/resumability: PASS by design.** Tasks and evidence checkpoints make the
  small implementation resumable without touching formal old matrices.
- **ARS: not applicable.** This is a local implementation correction, not a
  literature review, paper-writing task, or statistical comparison.

No constitution exception is required.

## Source-Backed Design

### 1. Freeze a single video class schedule

Create one small UAV application value object, conceptually
`VideoSampleClassSchedule`, when a video session accepts its FPS. It contains
validated FPS, backend class mode, and—only for the exact GStreamer path—a key
interval and pure `classFor(sampleId)` operation.

Use the same frozen object for:

- `ensureFutureSampleAnnouncementsLocked`;
- the legacy announcement/publication choice of one bounded `opaque` class;
- validation of GStreamer's authoritative `DELTA_UNIT`-derived class.

The legacy byte pipe cannot prove access-unit identity, so it must not infer
key/delta from a sequence counter. Its optional FPS-derived GOP setting is
encoder configuration only. The `opaque` class uses a conservative hard extent
bound.

Do not relabel committed Mapping. A GStreamer flag that contradicts the
schedule is a session failure. An encoder restart starts at a clean IDR/session
boundary under a newly frozen schedule.

### 2. Put a fail-closed adapter at the C ABI

Both static GStreamer `appsink` entry points invoke a common exception-safe
adapter. The adapter:

1. calls the C++ handler;
2. catches `std::exception` and `...`;
3. atomically retains only the first sanitized, bounded error code/reason;
4. changes `Running` to `Failed`;
5. clears/suppresses subsequent application callback emission;
6. returns `GST_FLOW_ERROR`.

It must not synchronously call `stop()` from inside the GStreamer streaming
thread. The existing owner loop/destructor performs teardown. Repeated stop
remains safe.

### 3. Separate Core status from APP adaptation

The existing live-stream callback already receives
`LiveStreamStatus.fetchDecision`. Add a Ground Station snapshot protected by
the existing synchronization strategy and keyed by a monotonically increasing
consumer generation.

On an accepted active-generation status:

- copy the full Core decision fields needed by operator state;
- mark source `core-live-status`;
- preserve the status timestamp/generation.

On start, stop, replacement, or a status without a decision:

- clear availability;
- reject retired-generation callbacks;
- show explicit `unavailable` rather than application-computed transport
  values.

`VideoAdaptiveState.window`, `lookahead`, `interestLifetimeMs`, and
`missingTimeoutMs` become truthful Core-display fields. Additive fields expose
availability/source, phase, policy mode, capacity reason, and decision reason.
The existing APP bitrate decision and pressure/backlog fields remain APP-owned
and are labeled accordingly. `futureProbeLimit` remains an APP-configured cap
unless Core supplies an exact counterpart; it must not be described as a Core
decision.

### 4. Preserve Core and historical evidence

No change is planned under:

```text
ndn-service-framework/Stream.hpp
ndn-service-framework/Stream.cpp
ndn-service-framework/ServiceUser.cpp
ndn-service-framework/ServiceProvider.cpp
pythonWrapper/
specs/125-adaptive-sample-atomic-prefetch/
specs/126-loss-reorder-resilience/
results/spec125-adaptive-sample-atomic-20260719-confirm06/
results/spec126-loss-reorder-20260720-confirmation07/
```

If implementation appears to require one of these paths, stop and renew the
audit instead of silently broadening the feature.

## Project Structure

### Documentation

```text
specs/145-uav-video-runtime-corrections/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── traceability.md
├── tasks.md
├── checklists/requirements.md
├── contracts/
│   ├── runtime-correction-contract.md
│   └── frozen-baseline-contract.md
└── evidence/
    └── pre-implementation-audit.md
```

### Anticipated Implementation and Evidence Paths

```text
NDNSF-UAV-APP/
├── shared/
│   ├── UavProtocol.hpp
│   ├── UavProtocol.cpp
│   ├── UavVideoPipeline.hpp
│   └── UavVideoPipeline.cpp
├── drone/DroneServiceContainer.inc.hpp
└── ground-station/GroundStationServiceContainer.inc.hpp

tests/
├── unit-tests/
│   ├── uav-protocol-state.t.cpp
│   └── uav-video-pipeline.t.cpp
└── python/test_ndnsf_uav_unified_video.py

Experiments/
├── NDNSF_UAV_Stream_Control_Isolation_Campaign.py
└── analyze_spec145_uav_video_runtime.py

results/spec145-uav-video-runtime-<fresh-id>/
```

The exact existing test file may absorb a focused case instead of adding a new
file. No test-only production switch may alter normal runtime semantics.

## Implementation Phases

### Phase 0 - Freeze and audit

Record the four baseline digests, inspect the exact owner symbols, ensure no
historical process is active, and keep implementation blocked unless the
pre-implementation audit is PASS.

### Phase 1 - Class schedule and boundary tests

Write deterministic red tests for 20/30/60-fps GStreamer class schedules,
legacy conservative-class behavior and extent bound, actual encoder mismatch,
callback exceptions, failed-state suppression, and idempotent stop. Implement
the shared session class contract and callback adapter until these tests pass.

### Phase 2 - Truthful Core status

Write red tests for unavailable/active/stale decision snapshots and
`VideoAdaptiveState` field round trips. Add the generation-fenced snapshot and
separate Core/APP labels without changing bitrate adaptation.

### Phase 3 - Focused regression and fresh acceptance

Build only the affected targets first, then run existing native, unified
Python, and security gates. Freeze the exact new binary/config/command. Run one
fresh 60-second 20-fps GStreamer zero-loss MiniNDN cell once, analyze it, and
preserve any failure.

### Phase 4 - Closure and Spec 144 gate

Recompute frozen digests, audit changed symbols and ownership, and write the
post-implementation verdict. Only on PASS update Spec 144 documents to cite
the corrected UAV Video implementation; otherwise leave Spec 144 unchanged.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Encoder flag differs from predicted class | Fail current session; never mutate committed Mapping |
| Exception handler deadlocks teardown | Record failure only in callback; owner performs stop |
| Old status callback overwrites new session | Monotonic generation captured by callback |
| UI still mixes APP and Core state | Explicit availability/source fields and equality tests |
| Historical evidence changes accidentally | Before/after directory-root digests and no runner invocation |
| Small repair expands into Core redesign | Audit BLOCK on any generic Core/binding modification |
| Target UAV files already contain user edits | T001 snapshots the on-disk baseline; implementation patches only named symbols and never reverts unrelated hunks |

## Completion Definition

Spec 145 is complete only when T001-T006 are checked, the fresh cell has a
terminal preserved result, all success criteria are evaluated, the
post-implementation audit is PASS, and frozen digests are unchanged. Merely
writing this plan does not make UAV Video a Spec 144 reference.
