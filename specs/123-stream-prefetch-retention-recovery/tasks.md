# Tasks: Stream Prefetch Retention Recovery

**Input**: Design documents from `specs/123-stream-prefetch-retention-recovery/`

## Phase 1: Core correctness

- [x] T001 [US1] Make payload lifecycle and adaptive scheduling truthful by first adding failing eviction/future and decision-application cases, then implementing Provider classification plus consumer budget/range/lifetime use, and closing the focused Stream gate in `tests/unit-tests/stream.t.cpp` and `ndn-service-framework/Stream.{hpp,cpp}`.

## Phase 2: UAV live recovery

- [x] T002 [US2] Make UAV video retention and ordered access-unit progression duration-bounded by first adding focused retention-policy coverage, then implementing computed retention, explicit incomplete-frame drop reasons, and status fields in `tests/unit-tests/uav-protocol-state.t.cpp`, `NDNSF-UAV-APP/shared/UavProtocol.{hpp,cpp}`, `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`, and `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`.

## Phase 3: Real network acceptance

- [x] T003 [US2] Execute one unique 60-second zero-loss MiniNDN UAV run through the repaired Core, extend the existing analyzer only where required for per-bucket continuity/retention evidence, and apply every stop/pass gate without replacement in `Experiments/NDNSF_UAV_GUI_Minindn.py`, `Experiments/analyze_stream_latency.py`, and `results/spec123-*`.

## Phase 4: Closeout

- [x] T004 Re-run focused and relevant regression gates, audit implementation against the contracts, preserve honest positive or negative evidence, and update completion guidance in `specs/123-stream-prefetch-retention-recovery/completion-summary.md`, `NDNSF-UAV-APP/README.md`, `README.md`, and `README_ch.md` only for claims actually measured.

## Phase 5: Paper-aligned prefetch correction

- [x] T005 [US1] Reopen the Core claim and complete one cohesive paper-aligned correction: add failing tests for payload-expression-to-Data DRD, phase-driven cursor issuance, segmented-sample demand, and replacement Interest refill before APP processing; implement bounded network/processing ownership in `LiveStreamConsumerHandle`; restore the original 3600-byte/12+1 UAV load; pass focused regressions; execute unique preserved 60-second MiniNDN attempts through the accepted `results/spec123-paper-prefetch-sample-reserve-20260719-060053`; and replace premature closeout/audit claims only with measured evidence.

## Dependencies and execution order

- T001 is the blocking Core correctness gate.
- T002 depends on T001 and owns only UAV policy/assembly.
- T003 depends on T001 and T002.
- T004 depends on all implementation and evidence tasks.
- T005 supersedes the completion claim made by T004 and is complete only when its original-load network gate passes.

## Task cohesion audit

The five tasks are behavioral outcomes. T005 intentionally keeps its tests, Core implementation, original-load acceptance run, and evidence correction together because they falsify or close one prefetch claim; no task exists only to edit one file or run one command.
