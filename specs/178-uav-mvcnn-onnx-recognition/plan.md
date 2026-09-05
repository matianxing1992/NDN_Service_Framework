# Implementation Plan: UAV MVCNN ONNX Joint Recognition

**Branch**: `UAV-Experimental` | **Date**: 2026-08-29 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/178-uav-mvcnn-onnx-recognition/spec.md`

## Summary

Spec 178 replaces the claim-bounded Spec 177 CPU fallback with a real, digest-registered MVCNN-family ONNX artifact executed explicitly by ONNX Runtime's CPU provider. The selected NDNSF Provider will convert the already verified 2-6 view set into one bounded model input, execute joint view pooling inside the registered graph, publish the existing Provider-owned result and annotations, and retain the deterministic adapter only as a test double. A paired 1/2/4/6-view campaign will use the same artifact, preprocessing, samples, and CPU host; it may report an advantage only when the pre-registered analysis supports one.

## Technical Context

**Language/Version**: C++17 for the existing UAV/NDNSF contracts; Python 3.8 for model export, CPU inference, and evaluation

**Primary Dependencies**: current NDNSF dynamic runtime, ndn-cxx, NDN-SVS, ONNX Runtime 1.19.2 Python runtime explicitly restricted to `CPUExecutionProvider` on the reference host, ONNX 1.17.0, PyTorch 2.4.1+cpu for controlled checkpoint export, NumPy 1.24.4, Pillow 10.4.0, MiniNDN/NFD

**Storage**: immutable external model artifact plus a tracked small registration/manifest; generated model, dataset, logs, and campaign outputs remain under content-addressed local caches or ignored `results/`

**Testing**: ONNX checker and model-contract tests, pytest unit/model/evaluation tests, existing Boost.Test C++ unit/integration gates, real MiniNDN multi-process scenarios, paired registered evaluation

**Target Platform**: Linux x86_64; initial reference host has four logical CPUs and exposes `CPUExecutionProvider` (plus Azure in the installed wheel, which the qualification path must not select)

**Project Type**: existing C++ NDNSF/UAV application with a bounded Python model worker and offline evaluation tools

**Performance Goals**: at least 95% of qualified functional jobs finish within the existing five-second global deadline on the registered CPU host; report model-only and end-to-end latency separately

**Constraints**: one immutable model artifact; explicit CPU-only provider selection; no download or deterministic fallback during a real-model run; 2-6 operational views with an explicit one-view baseline; no image bytes in NDNSF service payloads; preserve Spec 176/177 security and terminal ownership

**Scale/Scope**: one target and one selected Provider per request-scoped job, at most six image views, paired 1/2/4/6-view treatment rows per registered target, one functional artifact-qualification dataset plus one vehicle-oriented quantitative dataset

## Constitution Check

*GATE: PASS before research and PASS after design.*

| Principle | Result | Plan evidence |
| --- | --- | --- |
| Canonical Dynamic Runtime | PASS | Reuses the existing unified UAV service path and Spec 177 job/result types; no generated stubs or split service names. |
| Security Is Part Of The Data Path | PASS | View admission, selected-Provider ownership, NAC-ABE authorization, permission, token, replay, signer, and digest checks remain before inference. |
| CodeGraph First | PASS | Current worker and existing ONNX Runtime adapter patterns were inspected before planning. |
| Spec-Driven Change | PASS | Spec, research, data model, contract, and ordered validation guide precede implementation. |
| Verify With The Right Scope | PASS | Validation order is contract/unit -> real CPU model -> CPU integration -> MiniNDN -> paired campaign. |
| Cohesive Outcome-Based Tasks | PASS | T001-T012 close artifact qualification, real inference, NDNSF integration, and paired evidence as behavioral slices rather than per-file bookkeeping. |

No constitution violation requires a complexity exception.

## Technical Decisions

1. **Real model subject**: qualify one legally usable MVCNN-family checkpoint and export a single ONNX graph. The current `.onnx` and `.pt` subjects are hash-pinned and remain qualification-only; no UAV-domain accuracy is pre-claimed.
2. **Bounded joint input**: prefer one graph with a maximum of six view slots and an explicit mask. The same learned weights serve 1/2/4/6-view treatments, and masked slots cannot influence max pooling or logits.
3. **Auditable outputs**: expose fused logits and the pooled representation as graph outputs. Digest those tensors together with the input mask and ordered exact Data references.
4. **CPU qualification**: create the session with only `CPUExecutionProvider`, disable runtime fallback where supported, and reject any session whose active provider list or profiling evidence does not match the registration.
5. **No silent substitution**: the Spec 177 deterministic adapter remains a separate functional-only profile. Artifact, export, provider, or inference failures terminate the real-model attempt.
6. **Two evidence ladders**: use the model's native rendered-view dataset to qualify export parity and joint inference; use a synchronized vehicle-oriented dataset for any UAV-domain quality claim. Generated car images remain a transport/function smoke only.
7. **Fair comparison**: one-view and 2/4/6-view rows share artifact, class map, preprocessing, target set, subset rule, host, deadline, and measurement definitions. Report negative and inconclusive outcomes.

## Validation Gates

| Gate | Subject | Required evidence |
| --- | --- | --- |
| G1 | Registry and ONNX contract | License/source/digest, checker pass, exact I/O names/shapes/types, view-mask semantics, class map |
| G2 | Real CPU model smoke | Explicit CPU provider, export parity, 1/2/4/6 completion, permutation equivalence, no fallback |
| G3 | UAV CPU integration | Exact verified view set reaches one graph run; result and annotations bind to the model evidence |
| G4 | Negative/security matrix | Bad model/view/provider/replay/output cases fail at registered stages; Spec 176/177 regressions pass |
| G5 | Real MiniNDN | Multiple UAV producers, two eligible Providers, one selected CPU inference owner, exact result delivery |
| G6 | Paired evaluation | Frozen dataset/model/config, complete 1/2/4/6 rows, latency/bytes/memory, effect size and uncertainty |
| G7 | Final audit | Requirement-to-test-to-evidence mapping with no claim beyond the measured subject |

G5 cannot replace G1-G4. G6 cannot begin until G5 passes with the exact candidate model registration.

## Project Structure

### Documentation (this feature)

```text
specs/178-uav-mvcnn-onnx-recognition/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
├── contracts/
│   └── mvcnn-onnx-model.md
└── tasks.md                 # generated in the next Spec Kit phase
```

### Source Code (repository root)

```text
NDNSF-UAV-APP/
├── configs/
│   └── uav_multiview_models.json     # extend with immutable real-model profile
├── shared/
│   ├── UavMultiViewRecognition.*     # retain generic evidence/result contract
│   └── UavDetectorProvider.*         # selected-Provider real-model boundary
├── ground-station/
│   └── UavIncidentCoordinator.*      # validate returned model/output evidence
├── tools/
│   ├── mvcnn_onnx_worker.py           # bounded CPU ONNX session
│   ├── export_mvcnn_onnx.py           # reproducible checkpoint export/verification
│   ├── run_multiview_fixture.py       # real-model smoke plus explicit fake mode
│   ├── run_uav_multiview_minindn.py   # real-model transport scenarios
│   └── evaluate_multiview_recognition.py
└── testdata/
    └── multiview-car/                 # functional-only fixture

tests/
├── unit-tests/uav-multiview-recognition.t.cpp
├── integration-tests/uav-multiview-flow.t.cpp
├── python/test_uav_mvcnn_onnx_*.py
└── minindn/test_uav_multiview_recognition.py
```

**Structure Decision**: Keep the ONNX worker in the existing UAV application boundary. Reuse the Spec 177 wire/job/result types and Provider/coordinator ownership; do not route the application through the stateful LLM-oriented distributed-inference ONNX adapter merely because both use ONNX Runtime.

## Complexity Tracking

No constitution violation or additional subsystem is introduced. The real model adapter replaces a test-only execution subject while retaining existing contracts and lifecycle ownership.
