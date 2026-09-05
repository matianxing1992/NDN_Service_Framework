# Tasks: UAV MVCNN ONNX Joint Recognition

**Input**: Design documents from `specs/178-uav-mvcnn-onnx-recognition/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/mvcnn-onnx-model.md`

**Tests**: Required. Preserve the order model contract -> real CPU ONNX -> UAV CPU integration -> MiniNDN -> paired campaign.

## Phase 1: Reproducible Model Subject

**Purpose**: Establish an immutable, legally usable model subject before application code can claim real MVCNN execution.

- [X] T001 Identify one MVCNN-family source/checkpoint with verified code and weight terms, reproduce its native CPU evaluation on the model's own view dataset, and freeze source revision, checkpoint/dataset hashes, class map, preprocessing, cache location, and acquisition manifest with a failing-then-passing provenance test in `NDNSF-UAV-APP/configs/uav_multiview_models.json`, `NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py`, `tests/python/test_uav_mvcnn_onnx_provenance.py`, and `specs/178-uav-mvcnn-onnx-recognition/evidence/model-provenance.md`

---

## Phase 2: Foundational ONNX Qualification

**Purpose**: Produce and certify the exact graph/input/runtime contract used by every later story.

**CRITICAL**: No UAV integration or experiment begins until both tasks pass.

- [X] T002 Export the qualified checkpoint as one maximum-six-view ONNX graph with explicit mask, fused logits, and pooled-feature outputs; prove native/ONNX parity, masked-slot neutrality, 1/2/4/6 compatibility, permutation equivalence, and exact artifact hashing with test-first coverage in `NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py`, `tests/python/test_uav_mvcnn_onnx_contract.py`, and `specs/178-uav-mvcnn-onnx-recognition/evidence/onnx-qualification.md`
- [X] T003 Implement a fail-closed CPU-only MVCNN session that verifies artifact/I/O/class-map/preprocessing contracts, activates only `CPUExecutionProvider`, disables fallback, rejects malformed or non-finite inputs/outputs, and emits `MvcnnExecutionEvidence` with focused negative tests in `NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py`, `NDNSF-UAV-APP/tools/multiview_contract.py`, `tests/python/test_uav_mvcnn_onnx_inference.py`, and `NDNSF-UAV-APP/configs/uav_multiview_models.json`

**Checkpoint**: The exact real model runs reproducibly on CPU without NDNSF or MiniNDN and cannot silently become the Spec 177 test double.

---

## Phase 3: User Story 1 - Real Joint MVCNN Recognition (Priority: P1) MVP

**Goal**: One selected Provider jointly consumes the verified 2-6 view set in the real graph and produces one auditable fused result.

**Independent Test**: The generated six-view fixture reaches one CPU ONNX inference with six true input slots, one pooled representation, one fused decision, six annotations, and `fallbackUsed=false`; view permutation is equivalent and view removal/replacement changes registered fusion evidence.

- [X] T004 [US1] Replace implicit fallback behavior with explicit real-model versus functional-adapter profiles, route the accepted Spec 177 view set through the strict MVCNN worker, and close 1/2/4/6-view, repeatability, permutation, sensitivity, deadline, and no-fallback gates in `NDNSF-UAV-APP/tools/multiview_recognition_worker.py`, `NDNSF-UAV-APP/tools/run_multiview_fixture.py`, `tests/python/test_uav_multiview_model.py`, and `tests/python/test_uav_mvcnn_onnx_inference.py`
- [X] T005 [US1] Bind real model evidence to the existing selected-Provider execution, fused result, Provider-owned annotation references, and coordinator acceptance while preserving the Spec 177 wire/job contract; close the CPU integration gate in `NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp`, `NDNSF-UAV-APP/shared/UavMultiViewRecognition.cpp`, `NDNSF-UAV-APP/shared/UavDetectorProvider.hpp`, `NDNSF-UAV-APP/shared/UavDetectorProvider.cpp`, `NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.cpp`, and `tests/integration-tests/uav-multiview-flow.t.cpp`

**Checkpoint**: User Story 1 is independently demonstrable on one CPU host with a real artifact and no accuracy claim from generated images.

---

## Phase 4: User Story 2 - NDNSF Security and Delivery (Priority: P2)

**Goal**: The real model remains behind the current NDNSF admission, selection, and named-Data boundaries and has one terminal owner.

**Independent Test**: Invalid model/view/provider/replay cases fail before or at their registered model stage; a real MiniNDN topology shows one selected CPU Provider executing the artifact and all other Providers producing no terminal outputs.

- [X] T006 [US2] Extend the deterministic security/failure matrix to real-model provenance, provider selection, wrong signer/digest, replay, duplicate/cross-target views, invalid mask, non-selected output, inference timeout, invalid outputs, and incomplete annotations without bypassing existing NDNSF checks in `tests/python/test_uav_multiview_security.py`, `tests/python/test_uav_mvcnn_onnx_security.py`, `tests/unit-tests/uav-multiview-recognition.t.cpp`, and the corresponding `NDNSF-UAV-APP/shared/` implementation
- [X] T007 [US2] Run the actual MVCNN CPU worker inside the reusable MiniNDN multi-process fixture, add model-artifact/provider/inference failure scenarios alongside the existing nominal/selection/view/publication cases, and retain exact model/input/result hashes and one-owner lifecycle evidence in `NDNSF-UAV-APP/tools/minindn_multiview_node.py`, `NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py`, `tests/minindn/test_uav_multiview_recognition.py`, and `specs/178-uav-mvcnn-onnx-recognition/evidence/minindn-matrix.md`

**Checkpoint**: MiniNDN proves transport, security, selected execution, and distinct network/model failure stages with the exact qualified artifact.

---

## Phase 5: User Story 3 - Paired Recognition Evaluation (Priority: P3)

**Goal**: Determine, without assuming the outcome, whether jointly using more views improves recognition for the registered subject and data.

**Independent Test**: Every eligible target has immutable paired 1/2/4/6-view rows or an explicit exclusion; the report includes quality, latency, bytes, memory, effect size, confidence intervals, and transparent negative/inconclusive results.

- [X] T008 [P] [US3] Register the model-native export-qualification dataset and one synchronized vehicle-oriented multi-view evaluation dataset with license, target grouping, ground truth, deterministic 1/2/4/6 subsets, preprocessing/crop policy, sample-size rationale, and hashes in `NDNSF-UAV-APP/configs/uav_mvcnn_onnx_evaluation.json`, `NDNSF-UAV-APP/tools/prepare_coperception_uav.py`, `tests/python/test_uav_mvcnn_dataset_registration.py`, and `specs/178-uav-mvcnn-onnx-recognition/evidence/dataset-registration.md`
- [X] T009 [US3] Extend the evaluator to require the real-model profile and produce complete paired observations, top-1 accuracy, macro-F1 where valid, completion, confidence, model/end-to-end latency, bytes, accepted views, peak RSS, failure counts, paired deltas, confidence intervals, and McNemar evidence without outcome-dependent exclusions in `NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py` and `tests/python/test_uav_mvcnn_onnx_evaluation.py`
- [X] T010 [US3] Execute the frozen paired 1/2/4/6-view campaign only after T001-T009 pass, preserve the manifest and result hashes, and write a claim-neutral interpretation that distinguishes export validity, UAV-domain quality, and resource cost in `results/uav-mvcnn-onnx-*` and `specs/178-uav-mvcnn-onnx-recognition/evidence/paired-evaluation.md`

**Checkpoint**: The evidence can support a bounded positive, negative, or inconclusive conclusion without changing the registered analysis.

---

## Phase 6: User Story 4 - Explicit Baseline Separation (Priority: P4)

**Goal**: Retain fast functional and single-view paths without allowing them to contaminate real-model evidence.

**Independent Test**: Real MVCNN, deterministic Spec 177, and Spec 176 single-view runs have distinct profile identifiers, evidence fields, allowed claims, and regression gates; a failed real model never switches profiles.

- [X] T011 [US4] Enforce explicit real/fake/single-view profile selection, prohibit automatic fallback in real-model commands, retain Spec 176/177 regressions, and synchronize English/Chinese usage and claim boundaries in `NDNSF-UAV-APP/configs/uav_multiview_models.json`, `NDNSF-UAV-APP/tools/run_multiview_fixture.py`, `NDNSF-UAV-APP/README.md`, `NDNSF-UAV-APP/README_ch.md`, and `tests/python/test_uav_mvcnn_profile_separation.py`

---

## Phase 7: Final Audit and Release Gate

**Purpose**: Prove requirement coverage and prevent a functional model run from becoming an unsupported recognition claim.

- [X] T012 Audit FR-001-FR-022 and SC-001-SC-009 against current code and retained evidence, rerun G1-G7 in order, resolve every Spec178 BLOCK finding, and record exact passes, unrelated repository failures, residual risks, and the final claim boundary in `specs/178-uav-mvcnn-onnx-recognition/traceability.md`, `specs/178-uav-mvcnn-onnx-recognition/AUDIT.md`, and `specs/178-uav-mvcnn-onnx-recognition/evidence/release-gate.md`

---

## Dependencies and Execution Order

```text
T001 -> T002 -> T003
                 -> T004 -> T005
                 -> T006 -> T007
                 -> T008 -> T009
T005 + T007 + T009 -> T010
T003 -> T011
T001-T011 -> T012
```

- T001-T003 are hard prerequisites for every real-model claim.
- T004-T005 close the MVP real CPU model behavior.
- T006 may begin after T003 while T004/T005 stabilize, but T007 requires the integrated real worker.
- T008 may proceed in parallel after the model subject is known; T009 requires the final profile/evidence schema.
- T010 is blocked until real MiniNDN and paired evaluator gates pass with the same artifact registration.

## Parallel Opportunities

- T006 negative/security cases and T008 dataset registration can proceed in parallel after T003 because they own different artifacts and acceptance gates.
- T011 baseline separation can proceed after T003 while the MiniNDN/evaluation paths mature, provided it does not change the real-model evidence schema.

## Implementation Strategy

### MVP

Complete T001-T005. This yields a real MVCNN-family ONNX artifact executing joint 2-6-view inference on CPU with NDNSF result provenance, while generated images remain functional-only.

### Research Extension

Complete T006-T012. This adds real MiniNDN security/delivery evidence, paired 1/2/4/6-view evaluation, explicit baseline separation, and a reviewer-defensible claim boundary.

## Task Cohesion Review

The list contains 12 behavioral outcomes. Export plus parity is one task; CPU session plus negative contract is one task; Provider/coordinator integration plus its focused gate is one task; each campaign includes its registration, execution, and retained interpretation only when those parts have no independent acceptance value. No test/implementation/run/evidence chain is mechanically split.
