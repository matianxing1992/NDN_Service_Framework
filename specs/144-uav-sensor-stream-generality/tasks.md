# Tasks: UAV Sensor Stream Generality

**Input**: Design documents from
`/specs/144-uav-sensor-stream-generality/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md),
[research.md](research.md), [data-model.md](data-model.md),
[experiment-plan.md](experiment-plan.md), and [contracts/](contracts/)

**Tests**: Test-first deterministic, security, runner, analyzer, and MiniNDN
preflight gates are mandatory because this feature controls a formal
performance/reliability claim.

**Organization**: Nine cohesive behavioral outcomes. Formal campaigns are
explicit tasks because each produces independently meaningful immutable
evidence. Test, implementation, focused validation, and direct evidence remain
inside the same task.

**Reference implementation**: The corrected Spec 145 UAV Video path is the
formal APP-side integration reference. Every implementation task must preserve
its ownership pattern—session-frozen APP announcement/publication truth,
existing Core `AdaptiveSampleAtomic` consumption, exact-name/security
admission, fail-closed callback containment, and generation-fenced actual Core
status—without copying video class, key/delta, FPS/GOP, codec, or payload
semantics.

## Phase 1: Setup and Audit

**Purpose**: Establish immutable baselines and a blocking architecture/evidence
gate before source implementation.

- [X] T001 Perform the pre-implementation audit by recomputing and recording the immutable Spec 127/128 root manifests; verifying the Spec 145 post-implementation PASS and exact promoted campaign-summary hash; using CodeGraph to inventory the current `announceSample`/`prepareSampleExtent`/`publishSample`, consumer, retry/recovery, binding, corrected UAV Video reference, and telemetry polling paths; grading necessity/ownership/security/migration/rollback/evidence readiness; and writing a PASS/BLOCK verdict with the anticipated changed-file set in `specs/144-uav-sensor-stream-generality/evidence/baseline-manifest.md` and `specs/144-uav-sensor-stream-generality/evidence/pre-implementation-audit.md`; BLOCK stops T002-T009, and any dependency on video/codec workload semantics is BLOCK.

---

## Phase 2: Foundational Evidence Infrastructure

**Purpose**: Make every requested metric and every formal invocation
conservative, joinable, and testable before either application workload exists.

**⚠️ CRITICAL**: T002 and T003 block all user-story work.

- [X] T002 Deliver the generic Interest/sample outcome measurement path by first adding failing conservation, percentile, clock-domain, Mapping-novelty, future-hit, retry/recovery, and three-way Interest-utility cases, then implementing the minimal generic unsampled terminal-attempt ledger/status/binding parity and analyzer joins required to pass them, and recording the focused neutrality result in `ndn-service-framework/Stream.hpp`, `ndn-service-framework/Stream.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `Experiments/analyze_spec144_uav_sensor_stream.py`, `tests/unit-tests/stream.t.cpp`, `tests/python/test_ndnsf_uav_sensor_stream_generality.py`, and `specs/144-uav-sensor-stream-generality/evidence/metric-contract.md`; retain existing sampled `TimelineTrace` diagnostics unchanged and do not use them as the conservation authority.
- [X] T003 Deliver the single-owner one-shot MiniNDN harness by first testing duplicate cell IDs, reused destinations, source/config/analyzer drift, qdisc mismatch, concurrent ownership, incomplete readiness/count/window, automatic retry, and formal mutation rejection, then implementing the two-node workload launcher plus 32-cell manifest runner and recording the passing harness contract in `Experiments/NDNSF_UAV_Sensor_Stream_Generality_Minindn.py`, `Experiments/run_spec144_uav_sensor_stream_matrix.py`, `tests/python/test_spec144_uav_sensor_stream_runner.py`, and `specs/144-uav-sensor-stream-generality/evidence/runner-contract.md`.

**Checkpoint**: Metric conservation and one-shot execution are independently
validated; neither workload has begun formal execution.

---

## Phase 3: User Story 1 - Fresh UAV Telemetry Stream (Priority: P1) 🎯 MVP

**Goal**: Add a real UAV-APP compact telemetry stream over the generic
single-item/no-FEC path while preserving the existing snapshot service.

**Independent Test**: A deterministic in-process/fixture run produces exactly
the 20 Hz, 256/384/512-byte cycle; Ground Station admits monotonic validated
samples, reports AoI, and continues to support `GetStatus`.

- [X] T004 [US1] Deliver compact UAV telemetry Streaming end to end by adding failing exact-size/cadence/identity/session/name/protection/monotonicity/late-sample/fallback cases, implementing the APP-owned telemetry payload and deterministic source plus provider `announceSample`/`publishSample` and latest adaptive consumer admission in the dedicated real UAV-APP provider/consumer executable, preserving the existing `GetStatus` path without changing its Drone/Ground Station containers, and recording focused deterministic/security results in `NDNSF-UAV-APP/shared/UavSensorStreams.hpp`, `NDNSF-UAV-APP/shared/UavSensorStreams.cpp`, `NDNSF-UAV-APP/tools/uav_sensor_stream_node.cpp`, `tests/unit-tests/uav-protocol-state.t.cpp`, `tests/python/test_ndnsf_uav_sensor_stream_generality.py`, and `specs/144-uav-sensor-stream-generality/evidence/telemetry-deterministic.md`.

**Checkpoint**: Telemetry is independently functional without audio and without
formal campaign evidence.

---

## Phase 4: User Story 2 - Short-Block Acoustic/Audio Stream (Priority: P2)

**Goal**: Add deterministic opaque 40 ms blocks with variable 2/3/4-source
extent and generic two-repair recovery, without codec/device/Core semantics.

**Independent Test**: Deterministic direct and zero/one/two/over-capacity loss
cases reconstruct exact ordered bytes, classify repair consumption, deliver
each complete block once, and fail closed on invalid data.

- [X] T005 [US2] Deliver the APP-owned acoustic/audio block stream by adding failing 2/3/4 exact-extent, two-erasure, over-capacity, malformed-repair, wrong-provider/session/name, duplicate/late recovery, protection-only, complete-block admission, and 512-byte whole-item boundary cases, implementing the deterministic/file-backed opaque source with provider announce/prepare/publish and latest adaptive consumer reconstruction in the dedicated real UAV-APP provider/consumer executable, and recording focused deterministic/security results in `NDNSF-UAV-APP/shared/UavSensorStreams.hpp`, `NDNSF-UAV-APP/shared/UavSensorStreams.cpp`, `NDNSF-UAV-APP/tools/uav_sensor_stream_node.cpp`, `tests/unit-tests/uav-protocol-state.t.cpp`, `tests/python/test_ndnsf_uav_sensor_stream_generality.py`, and `specs/144-uav-sensor-stream-generality/evidence/acoustic-deterministic.md`; no microphone, playback, codec, or Core semantic branch is permitted.

**Checkpoint**: Acoustic/audio is independently functional without telemetry
formal evidence.

---

## Phase 5: User Story 3 - Trustworthy Cross-Application Evidence (Priority: P3)

**Goal**: Freeze, execute, analyze, and close the new evidence without touching
or rerunning Specs 127/128.

**Independent Test**: A fresh output root contains exactly 32 unique
single-invocation terminal cells, complete raw-to-summary conservation,
independent workload verdicts, unchanged historical hashes, and zero neutrality
violation.

- [X] T006 [US3] Close the implementation/readiness gate by running the full native build, forced Python binding rebuild, full and focused unit/security/runner/analyzer suites, then one separately named zero-loss non-formal MiniNDN preflight per workload; resolve only reproducible pre-formal generic defects, rerun the pre-implementation audit after any Core change, verify metric conservation and cleanup, freeze exact source/binary/config/command/analyzer hashes and all thresholds, and record PASS/BLOCK in `specs/144-uav-sensor-stream-generality/evidence/preflight-and-freeze.md`; no formal cell may start on BLOCK.
- [X] T007 [US3] Execute the complete fresh telemetry formal treatment exactly once per declared cell under the single campaign owner, without selective repair or replacement, then generate all 16 immutable cell summaries plus the telemetry treatment table, distributions, Interest-utility attribution, exact intervals, failed-gate explanations, and positive/negative telemetry verdict in `results/spec144-uav-sensor-stream-<fresh-id>/` and `specs/144-uav-sensor-stream-generality/evidence/telemetry-formal.md`.
- [X] T008 [US3] Execute the complete fresh acoustic/audio formal treatment exactly once per declared cell under the same frozen binary/config/analyzer and sequential MiniNDN ownership, without selective repair or replacement, then generate all 16 immutable cell summaries plus the acoustic treatment table, latency/gap/recovery distributions, protection-only/nonproductive Interest attribution, exact intervals, failed-gate explanations, and positive/negative acoustic verdict in `results/spec144-uav-sensor-stream-<fresh-id>/` and `specs/144-uav-sensor-stream-generality/evidence/acoustic-formal.md`.
- [X] T009 [US3] Close Spec 144 by verifying the 32-row manifest and per-cell conservation; recomputing unchanged Spec 127/128 roots without invoking their runners; reverifying the promoted Spec 145 PASS and campaign-summary hash without invoking its runner; auditing every changed Core/binding symbol and selector with CodeGraph plus the neutrality negative tests; proving no video class, key/delta, FPS/GOP, codec, payload, or video-threshold semantics entered either new workload or any formal denominator; issuing independent and shared bounded verdicts without threshold changes; updating all requirement/task traceability and checked task states; and writing `specs/144-uav-sensor-stream-generality/evidence/neutrality-audit.md`, `specs/144-uav-sensor-stream-generality/completion-summary.md`, `specs/144-uav-sensor-stream-generality/audit.md`, and `specs/144-uav-sensor-stream-generality/traceability.md`.

**Checkpoint**: Spec closes as measured positive only if both workload families
pass; otherwise it closes COMPLETE / MEASURED NEGATIVE.

---

## Dependencies & Execution Order

```text
T001
  -> T002
  -> T003
  -> T004
  -> T005
  -> T006
  -> T007
  -> T008
  -> T009
```

- T001 is a blocking audit.
- T002 supplies shared metrics needed by both workloads.
- T003 supplies the runner used by preflight/formal execution.
- T004 and T005 are independently testable application slices but share UAV
  integration files, so the default execution order is sequential.
- T006 freezes one exact implementation and analyzer for both formal
  treatments.
- T007 and T008 are sequential because MiniNDN and cleanup have one owner and
  because both must use the same frozen subject.
- T009 depends on every terminal formal cell and cannot trigger a rerun.

## Parallel Opportunities

No task is marked `[P]`. Although some document or fixture work could be
performed concurrently, shared UAV files and single-writer evidence ownership
make sequential execution the safer default. Parallelism inside build/test
commands may use the environment-safe job count, but formal MiniNDN cells remain
single-owner and sequential.

## Implementation Strategy

### MVP

Complete T001-T004 to demonstrate that existing UAV telemetry can use the
generic live-stream path. Stop before formal claims.

### Full validation

Complete T005, pass/freeze T006, then execute T007 and T008 exactly once. T009
reports what happened; it does not optimize the result.

## Fragmentation Review

- Metric instrumentation, analyzer joins, tests, and direct evidence remain one
  behavior in T002.
- Runner safeguards, orchestration, parser tests, and direct evidence remain
  one behavior in T003.
- Each application slice keeps test-first work, implementation, focused gates,
  and direct evidence together in T004/T005.
- Formal telemetry and acoustic campaigns remain separate because each
  produces an independently meaningful frozen workload verdict.
- Final neutrality/history/claim closure remains one task because none of its
  artifacts is independently releasable.

## Notes

- Task count is diagnostic, not a completeness claim.
- Never mark T007/T008 complete from smoke, diagnostic, historical, or
  selectively rerun data.
- If a formal result exposes a defect, preserve it and define a new Spec.
