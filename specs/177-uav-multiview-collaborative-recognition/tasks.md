# Tasks: UAV Multi-View Collaborative Recognition

**Input**: Design documents from `specs/177-uav-multiview-collaborative-recognition/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/multiview-recognition.md`

**Tests**: Required. Preserve the order unit tests -> CPU integration -> real model -> MiniNDN -> controlled experiment.

## Phase 1: Setup and Reproducible Inputs

**Purpose**: Freeze inputs before implementation so a changing checkpoint or fixture cannot invalidate later evidence.

- [x] T001 Register the MVP algorithm, model/checkpoint license and digest, preprocessing profile, supported 2-6 view policy, and generated fixture hashes; add a failing manifest-validation test and make it pass in `NDNSF-UAV-APP/configs/uav_multiview_models.json`, `NDNSF-UAV-APP/testdata/multiview-car/manifest.json`, and `tests/python/test_uav_multiview_contract.py`

---

## Phase 2: Foundational Multi-View Contract

**Purpose**: Establish one canonical job/result contract and an algorithm boundary shared by all stories.

**CRITICAL**: No model or MiniNDN task starts before this phase passes.

- [x] T002 Define versioned `MultiViewRecognitionJob`, `ViewEvidenceReference`, `MultiViewModelProfile`, `FusedRecognitionResult`, annotated-view references, terminal statuses, validation, and wire-size bounds with test-first round trips in `NDNSF-UAV-APP/shared/UavProtocol.hpp`, `NDNSF-UAV-APP/shared/UavProtocol.cpp`, and `tests/unit-tests/uav-multiview-recognition.t.cpp`
- [x] T003 Introduce an algorithm-independent view-set execution interface and deterministic CPU fake that proves deduplication, target/time correlation, minimum view/distinct-producer policy, consumed-view provenance, and explicit failure stages while preserving the existing Spec 176 single-evidence execution as the registered single-view baseline in `NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp`, `NDNSF-UAV-APP/shared/UavMultiViewRecognition.cpp`, `NDNSF-UAV-APP/shared/UavDetectorProvider.hpp`, `NDNSF-UAV-APP/shared/UavDetectorProvider.cpp`, `tests/integration-tests/uav-multiview-flow.t.cpp`, and `tests/python/test_uav_multiview_integration.py`

**Checkpoint**: Multi-view semantics pass without a GPU, network, or real model.

---

## Phase 3: User Story 1 - Joint Recognition From Multiple UAV Views (Priority: P1) MVP

**Goal**: One bounded job fetches and verifies multiple UAV views and produces a decision that demonstrably consumes the view set jointly.

**Independent Test**: A 2-6-view CPU case completes with exact contributing-view provenance; one-view, duplicate-only, mixed-target, and out-of-window cases cannot report multi-view success.

- [x] T004 [US1] Extend the Spec 176 coordinator and selected-Provider path to create one bounded multi-view job, fetch exact signed Data, validate each view, and pass only the accepted immutable set to the algorithm boundary; close nominal and negative CPU integration gates in `NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp`, `NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.cpp`, `NDNSF-UAV-APP/shared/UavDetectorProvider.hpp`, `NDNSF-UAV-APP/shared/UavDetectorProvider.cpp`, and `tests/integration-tests/uav-multiview-flow.t.cpp`
- [x] T005 [US1] Implement the real detector-guided MVCNN-style worker with per-view detection/crops, shared feature extraction, permutation-invariant pooling, fused classification, 1/2/4/6-view execution, model digest, and fusion evidence; prove that removing or replacing a non-duplicate view changes the deterministic pooled-feature digest in `NDNSF-UAV-APP/tools/multiview_recognition_worker.py`, `NDNSF-UAV-APP/tools/run_multiview_fixture.py`, and `tests/python/test_uav_multiview_model.py`

**Checkpoint**: The generated fixture runs through a genuine multi-view fusion stage locally.

---

## Phase 4: User Story 2 - Verifiable Annotated Outputs (Priority: P2)

**Goal**: Publish one fused result manifest and one immutable annotated image per contributing view under Provider-owned names.

**Independent Test**: A six-view result yields six exact annotation references; missing, corrupted, wrongly signed, duplicated, or source-mismatched outputs make terminal acceptance incomplete.

- [x] T006 [US2] Add Provider-owned result/annotation naming, image annotation, segmented publication where required, result-manifest construction, and coordinator fetch/signature/digest/completeness validation with test-first output tamper cases in `NDNSF-UAV-APP/shared/UavNames.hpp`, `NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp`, `NDNSF-UAV-APP/shared/UavMultiViewRecognition.cpp`, `NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.cpp`, and `tests/python/test_uav_multiview_outputs.py`

**Checkpoint**: Human-inspectable outputs preserve named-Data provenance and cannot be partially accepted.

---

## Phase 5: User Story 3 - No-Camera Functional Demonstration (Priority: P3)

**Goal**: Reproduce a real algorithm and app execution without physical cameras while clearly bounding the evidence.

**Independent Test**: The checked-in manifest verifies, all six images decode, 1/2/4/6-view runs complete, six-view annotations are produced, and reports label the fixture functional-only.

- [x] T007 [US3] Make the six-view generated car fixture a reproducible executable gate, including hash checks, deterministic view subsets, output manifest, stage timings, annotation preview, and an explicit no-accuracy-claim marker in `NDNSF-UAV-APP/testdata/multiview-car/`, `NDNSF-UAV-APP/tools/run_multiview_fixture.py`, `tests/python/test_uav_multiview_contract.py`, and `tests/python/test_uav_multiview_model.py`
- [x] T008 [US3] Add a registered controlled-dataset adapter and paired one-view versus 2/4/6-view evaluator with fixed sample grouping, metrics, uncertainty, model/dataset provenance, and result hashes in `NDNSF-UAV-APP/tools/prepare_coperception_uav.py`, `NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py`, `NDNSF-UAV-APP/configs/uav_multiview_evaluation.json`, and `tests/python/test_uav_multiview_evaluation_contract.py`

**Checkpoint**: Functional evidence and quantitative evidence are mechanically separated.

---

## Phase 6: User Story 4 - NDNSF Security and Multi-Process Delivery (Priority: P4)

**Goal**: Demonstrate that multi-view collaboration preserves current authorization and has one terminal owner over a real NDN topology.

**Independent Test**: Multiple UAV producers and at least two eligible Providers run in MiniNDN; only the selected Provider publishes the accepted result, while signer/digest/replay/mixed-target/timeout failures reach the registered terminal stage.

- [x] T009 [US4] Close unit and CPU integration security cases for wrong signer, bad digest, unauthorized service/model profile, replayed job/token, duplicate Data, mixed target/window, unselected Provider output, and incomplete annotations without bypassing existing NDNSF checks in `tests/unit-tests/uav-multiview-recognition.t.cpp`, `tests/python/test_uav_multiview_security.py`, and the corresponding `NDNSF-UAV-APP/shared/` implementation files
- [x] T010 [US4] Build one reusable MiniNDN fixture with multiple UAV image producers, coordinator, controller, and competing compute Providers; cover nominal, Provider-selection, unavailable-view, late-view, and publication-failure cases and preserve lifecycle/trace evidence in `NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py`, `tests/minindn/test_uav_multiview_recognition.py`, and `specs/177-uav-multiview-collaborative-recognition/evidence/`

**Checkpoint**: Unit and CPU integration gates pass before MiniNDN; MiniNDN passes before any simulator or cluster campaign.

---

## Phase 7: Evaluation and Documentation Closure

**Purpose**: Produce only defensible claims and keep application documentation synchronized.

- [x] T011 Run the registered paired controlled-dataset campaign only after T001-T010 pass, record 1/2/4/6-view quality, completion, latency, bytes, accepted-view counts, failure stages, uncertainty, manifests, and hashes, and write a claim-bounded report in `results/uav-multiview-*/` and `specs/177-uav-multiview-collaborative-recognition/evidence/controlled-evaluation.md`
- [x] T012 [P] Update English and Chinese UAV app documentation with the two-lifecycle workflow, multi-view versus independent-view distinction, generated-fixture limitations, model registration, validation order, and reproduction commands in `NDNSF-UAV-APP/README.md`, `NDNSF-UAV-APP/README_ch.md`, and `specs/177-uav-multiview-collaborative-recognition/quickstart.md`
- [x] T013 Audit requirement-to-test-to-evidence traceability, rerun the complete release gate, resolve every BLOCK finding, and record the final verdict in `specs/177-uav-multiview-collaborative-recognition/traceability.md`, `specs/177-uav-multiview-collaborative-recognition/AUDIT.md`, and `specs/177-uav-multiview-collaborative-recognition/evidence/release-gate.md`

---

## Dependencies and Execution Order

```text
T001
  -> T002 -> T003
              -> T004 -> T005
                        -> T006
                        -> T007 -> T008
                        -> T009 -> T010
                                  -> T011
  T012 may start after T005 and closes after T011
  T013 depends on every selected scope task
```

- T001-T003 are mandatory foundations.
- T005 needs the accepted-view contract from T003/T004.
- T006 can begin after the result contract and model adapter outputs stabilize.
- T008 is independent of NDN transport after T005/T007 but must use the same frozen model profile.
- T010 cannot substitute for unit or integration gates.
- T011 is blocked until MiniNDN and dataset registration pass.

## Parallel Opportunities

- After T005, T006 output publication and T008 controlled-dataset preparation can proceed in parallel because they use stable contracts and different files.
- T009 deterministic security tests may proceed alongside T007 fixture hardening after T004.
- T012 documentation can begin after behavior stabilizes but must be finalized after evidence exists.

## Implementation Strategy

### MVP

Complete T001-T007. This yields a real multi-view algorithm on the generated fixture, exact NDNSF input/output contracts, and verifiable annotated images without claiming quantitative accuracy.

### Research extension

Complete T008-T013. This adds controlled paired evaluation, MiniNDN delivery evidence, security closure, and publication-ready claim boundaries.

## Task Cohesion Review

The list contains 13 behavioral outcomes. Tests, implementation, focused validation, and evidence stay in the same task when they close one behavior. No task exists solely to add a file, run a command, or record bookkeeping.
