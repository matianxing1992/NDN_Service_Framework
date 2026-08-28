# Tasks: NDNSF Stream Latency Attribution and Continuity

**Input**: Design documents from `specs/121-ndnsf-stream-latency-attribution/`

**Tests**: Test-first work is included inside each behavioral task. Each task owns its focused evidence and is not split mechanically by file or command.

## Phase 1: Diagnostic Foundation

**Purpose**: Turn the observed stall and invalid one-second aggregate into deterministic pass/fail signals.

- [X] T001 Build the deterministic diagnostic loop by adding a three-Mapping-block consumer fixture, numeric publication/media cursor collision fixture, one-to-many decoder fixture, and bounded progress assertions in `tests/unit-tests/stream.t.cpp`, `tests/unit-tests/uav-protocol-state.t.cpp`, `tests/python/test_ndnsf_stream_latency.py`, and `Experiments/analyze_stream_latency.py`; prove the current stale-frontier and false-correlation behavior fails before implementation, and preserve the current Spec 120 run as read-only baseline evidence (FR-001 through FR-009; SC-003 through SC-007).

**Checkpoint**: The exact continuity failure and measurement invalidity reproduce without a full MiniNDN run.

---

## Phase 2: User Story 1 - Continuous Live Consumption (Priority: P1) MVP

**Goal**: A latest-mode consumer continues across Mapping blocks and never waits for intentionally skipped pre-join media.

**Independent Test**: The deterministic consumer crosses three Mapping blocks and the UAV reorder path accepts a nonzero first source sequence without synthetic loss.

- [X] T002 [US1] Make verified Mapping frontier handoff atomic and observable by updating adaptive scheduling from resolver frontiers, catching/reporting callback-boundary progress violations, and exposing bounded starvation diagnostics in `ndn-service-framework/Stream.hpp`, `ndn-service-framework/Stream.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/streaming.py`, and `tests/unit-tests/stream.t.cpp`; retain API/wire compatibility and make the T001 cross-block test pass (FR-001, FR-002, FR-009, FR-010, FR-014; SC-001 through SC-003, SC-010).
- [X] T003 [US1] Initialize UAV decoder reorder from the first verified source-media item at or after the safe join boundary, keeping publication cursor, FEC repair cursor, source-media sequence, empty shard, and key-frame behavior distinct in `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, and `tests/unit-tests/uav-protocol-state.t.cpp`; prove no pre-join timeout is charged and no repair item enters the decoder (FR-003, FR-004, FR-010; SC-003, SC-010).

**Checkpoint**: Core and UAV focused tests prove continuous legal progress and correct latest join.

---

## Phase 3: User Story 2 - Trustworthy Latency Attribution (Priority: P2)

**Goal**: Every reported interval has a compatible source identity, cardinality, clock domain, and startup/steady classification.

**Independent Test**: The analyzer rejects all deliberate cursor collisions and unrelated decoder pairings, while a valid fixture produces complete startup and steady distributions.

- [X] T004 [US2] Replace numeric-cursor and one-input/one-output assumptions with the stable correlation contract across sampled Core/UAV `NDN_LOG` events, decoder/GUI attribution, and the evidence analyzer in `ndn-service-framework/Stream.hpp`, `ndn-service-framework/Stream.cpp`, `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp`, `Experiments/NDNSF_UAV_GUI_Minindn.py`, `Experiments/analyze_stream_latency.py`, and `tests/python/test_ndnsf_stream_latency.py`; report startup, warmup, steady, missing, ambiguous, and cross-clock-unavailable samples without guessing (FR-004 through FR-009, FR-013; SC-004 through SC-007, SC-009).

**Checkpoint**: The prior approximately one-second aggregate can no longer be produced from mismatched identities or cold-start samples.

---

## Phase 4: User Story 3 - Evidence-Driven Low Latency (Priority: P3)

**Goal**: Establish the corrected continuous baseline and retain only a repeatable low-latency candidate.

**Independent Test**: A 60-second zero-loss run continues through the final ten seconds, then matched candidate pairs meet the correctness and benefit gates.

- [X] T005 [US3] Run one unique 60-second corrected MiniNDN baseline and close the continuity/measurement gate in `Experiments/NDNSF_UAV_GUI_Minindn.py`, `Experiments/analyze_stream_latency.py`, and `results/spec121-continuity-baseline-*/`; record exact command/environment, provider and consumer frontiers, final-ten-second delivery, future-hit and unnecessary-Interest counts, valid correlation rate, startup/steady distributions, CPU/memory/PIT/queue bounds, and preserve a terminal failure without rerun if any P1/P2 criterion fails (FR-007 through FR-013; SC-001 through SC-007).
- [X] T006 [US3] Select the highest measured remaining stage and implement exactly one candidate—provider encoder-pipe/packetization batching, decoder cold-start/lifecycle, FEC grouping, or adaptive lead/window—in its owning Core or UAV files with a failing focused regression and rollback switch; reject the candidate if the corrected evidence does not implicate that stage or if it changes protocol/security semantics (FR-010 through FR-014; SC-008, SC-010).
- [X] T007 [US3] Close Spec 121 without a default or repeatability claim: preserve the one baseline/candidate pair and its 8.3-times Interest-work cost, record that exact H.264 source-to-output attribution is unavailable, withdraw the original five-pair proxy-metric campaign, and transfer fresh counterbalanced default/trace acceptance ownership to Spec 122 in `completion-summary.md`, `experiment-summary.json`, `traceability.md`, and the post-implementation audit (FR-010 through FR-014; SC-001 through SC-010).

---

## Dependencies & Execution Order

- T001 is the required feedback loop.
- T002 and T003 depend on T001 and together close User Story 1.
- T004 depends on the stable source/join identities from T002/T003.
- T005 depends on T002 through T004 and blocks optimization.
- T006 is chosen only from T005 evidence.
- T007 is a governance closeout triggered by the T006 attribution limit. It does not execute or claim the superseded five-pair matrix; Spec 122 T009 owns new confirmatory cells after exact frame identity exists.

## Task Cohesion Audit

- Seven tasks represent seven independently reviewable outcomes: repro, Core continuity, APP join, correlation, corrected baseline, one candidate, and matched closeout.
- Test, implementation, focused validation, and evidence for one behavior remain within the same task.
- No task exists solely for one file, command, or document update.

## Implementation Strategy

1. Complete T001–T003 before any performance matrix.
2. Complete T004 before trusting latency percentiles.
3. Treat T005 as the corrected baseline, not an optimization result.
4. Let T005 evidence choose T006; do not tune multiple variables.
5. Preserve negative evidence and stop if correctness, security, or continuity regresses.
6. Do not reuse Spec 121 exploratory cells as Spec 122 confirmatory evidence.
