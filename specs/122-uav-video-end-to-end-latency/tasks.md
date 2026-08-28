# Tasks: UAV Video True End-to-End Latency

**Input**: Design documents from `specs/122-uav-video-end-to-end-latency/`

**Tests**: Test-first implementation and focused evidence are included inside each cohesive behavioral task.

## Phase 1: Diagnostic Foundation

**Purpose**: Establish a backend-independent identity oracle and reject false end-to-end measurements before changing runtime behavior.

- [X] T001 Build the deterministic 30 FPS frame-identity feedback loop by generating an H.264-resilient in-image frame marker and acquisition timestamp, recovering it from decoded frames as an independent oracle, and extending exact/ambiguous/drop fixtures and the analyzer in `Experiments/generate_uav_latency_source.py`, `Experiments/analyze_stream_latency.py`, `tests/python/test_ndnsf_stream_latency.py`, and `tests/fixtures/uav-video-latency/`; first prove the current trace cannot bind legacy decoder output to acquisition, then require exact oracle/runtime agreement and reject FIFO, cross-clock, percentile-sum, stale-session, and one-to-many guesses (FR-001 through FR-006; SC-001 through SC-003).

**Checkpoint**: The same source frame can be identified before encoding and after GUI decode without depending on either candidate backend's internal metadata.

---

## Phase 2: User Story 1 - Trustworthy Capture-to-Presentation Measurement (Priority: P1) MVP

**Goal**: Runtime and evidence preserve one exact frame identity and report every available stage honestly.

**Independent Test**: A deterministic stream produces exact source-to-widget samples for at least 95% of sampled displayed frames, with deliberate ambiguity and unsafe clocks reported unavailable.

- [X] T002 [US1] Introduce the APP-owned media pipeline and timing boundary while preserving current behavior: add test-first lifecycle/capability/queue contracts and a `legacy-pipe` adapter in `NDNSF-UAV-APP/shared/UavVideoPipeline.hpp`, `NDNSF-UAV-APP/shared/UavVideoPipeline.cpp`, `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp`, `wscript`, and `tests/unit-tests/uav-protocol-state.t.cpp`; prove bounded idempotent stop, explicit backend selection/fallback reason, bounded queue/drop accounting, headless operation, and byte-equivalent legacy video before proceeding (FR-007, FR-009, FR-010, FR-015; SC-007, SC-011).

- [X] T003 [US1] Select a viable timestamp backend through a focused, non-production capability gate in `NDNSF-UAV-APP/tools/uav_video_pipeline_probe.cpp`, `NDNSF-UAV-APP/tools/run_uav_video_pipeline_probe.sh`, `wscript`, and `tests/python/test_uav_video_pipeline_probe.py`: probe one selected GStreamer ABI first, verify plugins, PTS/access-unit round trip, appsink plus GTK GL/headless behavior, and five bounded start/stop cycles; only if it fails, run one bounded in-process libav PTS/lifecycle fallback probe. Record one PASS backend or stop Spec 122 as BLOCKED while keeping `legacy-pipe` operational but ineligible for production end-to-end attribution (FR-009, FR-010; SC-011).

- [X] T004 [US1] Deliver authenticated source-frame/PTS correlation through the backend selected by T003 by adding the failing binding/version/replay/PTS-conflict cases, additive UAV payload fields, exact stage events, decoder-output association, and widget-versus-presentation labels in `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, `NDNSF-UAV-APP/shared/UavVideoPipeline.*`, the Provider/Ground Station containers, `GroundStationWindow.inc.hpp`, `tests/unit-tests/uav-protocol-state.t.cpp`, and `tests/python/test_ndnsf_stream_latency.py`; close the story only when runtime identity agrees with the T001 oracle, malformed/stale bindings fail closed, sampled `NDN_LOG` is bounded, and Stream names/security/FEC remain unchanged. Do not execute this task if T003 selected no backend (FR-001 through FR-006, FR-011, FR-015; SC-001 through SC-003, SC-011).

**Checkpoint**: Capture-to-widget is a real per-frame measurement. Physical presentation remains unavailable unless the selected sink proves it.

---

## Phase 3: User Story 2 - Smooth Low-Latency Live Display (Priority: P2)

**Goal**: Use corrected stage evidence to remove the dominant startup and steady tail delays without accumulating stale frames.

**Independent Test**: A corrected 60-second baseline ranks the stages; retained one-variable candidates meet startup, output-gap, frame-age, and completion gates without weakening Stream behavior.

- [X] T005 [US2] Produce corrected exploratory 60-second MiniNDN baselines for both the exact-identity `legacy-pipe + stdio-batched` rollback reference and the provisional POSIX/20 ms code path, plus a ranked bottleneck report, by extending frozen configuration, stage/drop/queue/resource collection, validity gates, and canonical summaries in `Experiments/NDNSF_UAV_GUI_Minindn.py`, `Experiments/analyze_stream_latency.py`, `Experiments/run_spec122_video_latency_matrix.py`, and unique `results/spec122-baseline-*` directories; measure acquisition/encode/packetization/protection/network/validation/decrypt/reorder/decode/GUI endpoints, normalize work per produced displayable frame, classify unavailable presentation honestly, and stop before optimization unless identity coverage, continuity, security, future-hit, and frame accounting pass (FR-003 through FR-006, FR-012 through FR-014; SC-002, SC-003, SC-007 through SC-011).

- [X] T006 [US2] Resolve the dominant startup stage with ordered one-variable, test-first candidates in `NDNSF-UAV-APP/shared/UavVideoPipeline.*`, Provider/Ground Station integration, `Experiments/run_spec122_video_latency_matrix.py`, and `tests/unit-tests/uav-protocol-state.t.cpp`: probe persistent/prewarmed decoding first and authenticated codec-configuration/key-frame readiness second only if evidence requires it; retain at most one startup change that reaches the 150 ms gate without lifecycle, bitrate, completion, CPU, or rollback regression, otherwise preserve both as negative evidence (FR-008 through FR-010, FR-012 through FR-014; SC-004, SC-007, SC-009, SC-011).

- [X] T007 [US2] Resolve the dominant steady codec/packet stage with ordered one-variable candidates in `NDNSF-UAV-APP/shared/UavVideoPipeline.*`, `DroneServiceContainer.inc.hpp`, `GroundStationServiceContainer.inc.hpp`, `Experiments/run_spec122_video_latency_matrix.py`, and focused tests: evaluate access-unit-aligned batching and the timestamp-preserving GStreamer path only when T005 attributes delay there; retain at most one change that meets frame-age/output-gap gates while keeping future-hit at least 99%, Interest work within policy, and security/FEC/rollback intact, otherwise record a negative result (FR-007 through FR-014; SC-005 through SC-009, SC-011).

- [X] T008 [US2] Resolve GUI/backpressure delay independently from codec/transport by implementing and testing a bounded latest-frame mailbox, explicit stale/session-retired drop reasons, and then a direct video sink only if T005/T007 show conversion cost remains dominant in `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `GroundStationWindow.inc.hpp`, `NDNSF-UAV-APP/shared/UavVideoPipeline.*`, `tests/unit-tests/uav-protocol-state.t.cpp`, and Xvfb GUI fixtures; retain only behavior that prevents queue growth and improves exact frame age without presenting widget submission as scan-out or breaking headless mode (FR-004 through FR-010, FR-013; SC-002, SC-005 through SC-007, SC-009, SC-011).

**Checkpoint**: One evidence-selected combined configuration exists, but no default claim is made before the matched gate.

---

## Phase 4: User Story 3 - Evidence-Driven Backend and Default Selection (Priority: P3)

**Goal**: Decide whether the selected configuration is the new default, remains experimental, or is rejected.

**Independent Test**: Five matched baseline/candidate pairs and five trace-off/on pairs produce an admissible decision with all negative cells preserved.

- [X] T009 [US3] Freeze one selected configuration and execute the independent confirmatory decision using `Experiments/run_spec122_video_latency_matrix.py`, `specs/122-uav-video-end-to-end-latency/quickstart.md`, canonical `results/spec122-*` evidence, `specs/122-uav-video-end-to-end-latency/completion-summary.md`, `NDNSF-UAV-APP/README.md`, `README.md`, `README_ch.md`, and `docs/NDNSF-UAV/slides/main.{tex,pdf}`: exclude all Spec 121 and T005-T008 exploratory cells; freeze `B=legacy-pipe+stdio-batched` with exact identity and `C=selected configuration`; precommit order `B→C,C→B,B→C,C→B,B→C` and trace order `OFF→ON,ON→OFF,OFF→ON,ON→OFF,OFF→ON`; run each new unique cell once; apply every correctness/latency/hard 2-times Interest-work-per-produced-frame/resource gate; run zero-loss plus controlled-loss/FEC/reconnect/headless regressions; select accepted default or explicit experimental/negative status, preserve rollback, and publish only evidence-supported wording (FR-010 through FR-015; SC-004 through SC-011).

---

## Dependencies & Execution Order

- T001 is the mandatory measurement feedback loop.
- T002 and T003 depend on T001; T003 is an independent feasibility gate.
- T004 depends on T002 and one passing backend from T003. If neither GStreamer nor the bounded libav fallback probe passes, Spec 122 stops as BLOCKED and T004 does not start.
- T005 depends on exact identity from T004 and blocks all optimization.
- T006, T007, and T008 are executed in evidence-ranked order. Each changes one variable; a later task is skipped when its stage is not dominant.
- T009 depends on one admissible configuration from T006-T008; if none is retained, it closes with the provisional current code path and negative evidence, without manufacturing confirmatory cells or an accepted-default claim.

## Task Cohesion Audit

- Nine tasks represent nine independently reviewable outcomes: oracle, adapter boundary, capability gate, exact runtime correlation, corrected baseline, startup decision, steady pipeline decision, GUI decision, and matched closeout.
- Test-first work, implementation, focused validation, and evidence for one behavior remain in the same task.
- No task exists solely to edit one file, run one command, or update one document.
- Candidate ladders are kept within one stage-specific task because their alternatives share one bottleneck, owner, and acceptance decision; only one variable changes per experiment cell.

## Implementation Strategy

1. Complete T001–T004 before trusting any capture-to-display number.
2. Treat T005 as measurement, not optimization.
3. Let T005 rank T006–T008; do not run a broad factorial matrix.
4. Preserve failures and stop candidate families at their declared gate.
5. Change a runtime default only in T009 after matched evidence; otherwise retain explicit experimental or negative status.
