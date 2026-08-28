# Tasks: UAV Video Runtime Corrections

**Input**: [spec.md](spec.md), [plan.md](plan.md), contracts, and
[pre-implementation audit](evidence/pre-implementation-audit.md)

**Task rule**: Each task owns its tests, implementation, verification, and
direct evidence. Specs 125/126 are read-only evidence; their runners are
forbidden.

## Phase 1 - Blocking baseline and design gate

- [X] T001 Record the four frozen directory-root digests from
  `contracts/frozen-baseline-contract.md`; prove no Spec 125/126 MiniNDN runner
  or writer is active; use CodeGraph to refresh the owner/blast-radius map for
  `ensureFutureSampleAnnouncementsLocked`, `publishCurrentFrame`,
  `publishGStreamerAccessUnit`, both GStreamer sample callbacks,
  `currentVideoAdaptiveState`, `VideoAdaptiveState`, and the live-stream status
  callback; record current on-disk hashes/diffs for every overlapping dirty UAV
  file as the user-owned baseline and define the exact allowed hunks; rerun the
  Spec Kit pre-implementation audit and write
  `evidence/implementation-readiness.md`. Any generic Core/binding requirement
  or frozen digest drift is BLOCK.

**Checkpoint**: Source implementation cannot start until T001 records PASS.

## Phase 2 - FPS consistency and exception containment

- [X] T002 [US1] Add deterministic red tests covering 20/30/60-fps
  announced-versus-published classes for GStreamer exact access units and the
  legacy bounded conservative class, validated endpoint/invalid FPS, encoder
  restart and actual-class contradiction, then implement one session-frozen UAV
  video class contract used by future announcement/publication and GStreamer
  validation without synthesizing legacy key/delta identity; record focused
  test commands/results in `evidence/class-consistency-correction.md`.

**Checkpoint**: The deterministic class matrix passes at 20/30/60 fps with no
Core source change.

## Phase 3 - GStreamer exception containment

- [X] T003 [US2] Add injected standard/non-standard exception tests for
  capture and decode callbacks, then implement the common fail-closed C-ABI
  adapter with first-failure retention, `Failed` state, later-callback
  suppression, `GST_FLOW_ERROR`, and idempotent non-recursive teardown; record
  focused test commands/results in `evidence/callback-containment.md`.

**Checkpoint**: Every injected exception is contained and teardown remains
safe.

## Phase 4 - Actual Core state display

- [X] T004 [US3] Add red tests for unavailable, active, stopped,
  replacement, and retired-generation status snapshots plus complete
  `VideoAdaptiveState` field round trips; implement a generation-fenced cache of
  the actual active `LiveStreamStatus.fetchDecision`; populate Core display
  fields exactly from it, add explicit availability/source/phase/policy/capacity
  metadata, clear them when unavailable, and retain separate APP-owned bitrate,
  pressure, backlog, reorder, and configured-cap fields; verify the bitrate
  policy remains behaviorally unchanged and record evidence in
  `evidence/core-status-truth.md`.

**Checkpoint**: Tests prove exact Core equality and no APP-estimate fallback.

## Phase 5 - Regression and one fresh acceptance cell

- [X] T005 Build the affected C++ UAV targets; run the
  relevant native Stream/UAV protocol/video suites, unified Python UAV video
  tests, and UAV stream security contract; resolve only in-scope defects;
  verify with CodeGraph and source diff that generic Core, bindings, security,
  retry/FEC/Mapping algorithms, and frozen paths are unchanged; freeze exact
  source/binary/config/analyzer/command hashes; then execute exactly one new
  60-second, 20-fps, GStreamer, zero-loss, two-node MiniNDN acceptance cell in
  `results/spec145-uav-video-runtime-<fresh-id>/`, analyze every FR-016/FR-017
  metric, preserve its terminal outcome without replacement, and write
  `evidence/fresh-20fps-acceptance.md`.

**Checkpoint**: A failed cell remains failed evidence and blocks reference
promotion; it is not permission to rerun or tune the cell.

## Phase 6 - Closure and conditional Spec 144 promotion

- [X] T006 Recompute and compare the four frozen digests without invoking
  historical runners; audit all changed symbols for ownership, exception
  safety, stale-callback fencing, security, and absence of a manual fetch loop
  or duplicate prefetch policy; complete requirement/task traceability and
  write `evidence/post-implementation-audit.md` plus `completion-summary.md`.
  Only if every gate is PASS, update Spec 144's spec/plan/tasks/contracts to
  name the now-corrected UAV Video path as the formal reference implementation
  and record the exact Spec 145 completion evidence it depends on. If any gate
  fails, leave Spec 144 byte-for-byte unchanged and report the blocker.

## Dependencies and Execution Order

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006
```

No task is parallelized: the implementation touches shared UAV session and
status state, and MiniNDN has one writer. Build tools may use safe ordinary
parallelism.

## Scope Guard

- Do not edit or run Spec 125/126.
- Do not start the Spec 144 formal matrix.
- Do not change Core or bindings under this small repair.
- Do not revert or normalize pre-existing changes in overlapping UAV files.
- Do not optimize announcement lead, Mapping density, FEC, or retry.
- Do not mark T005 complete from unit tests or historical evidence.
- Do not promote Spec 144 before T006 PASS.
