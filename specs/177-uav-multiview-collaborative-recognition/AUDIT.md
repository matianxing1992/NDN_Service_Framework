# Spec 177 Code-Aware Pre-Implementation Audit

**Date**: 2026-08-29

**Branch**: `UAV-Experimental`

**Verdict**: **PASS — IMPLEMENTED AND VERIFIED WITH CLAIM-BOUNDED EVIDENCE.**

Spec 177 is necessary, bounded, source-supported, traceable, and aligned with the current NDNSF-UAV architecture after remediation.  All feature tasks and ordered gates now have implementation or retained evidence.  The generated/one-sample dataset remains explicitly functional only, so this PASS does not make a scientific accuracy or multi-view benefit claim.

## Baseline Truth

The pre-implementation baseline was the Spec 176 single-evidence path:
`UavDetectorProvider::execute(const UavVerifiedEvidence&)` accepted one
verified evidence object and returned a digest-only execution record.  Spec 176
did not provide a view set, pooled representation, or per-view annotated
outputs.  That baseline is retained as a regression path; it is not a
description of the current working tree.

The current working tree implements the bounded view-set contract, a
detector-guided MVCNN-style CPU worker with deterministic fallback, Provider-owned
output naming, coordinator acceptance, security rejection cases, a paired
controlled evaluator, and a real MiniNDN wire fixture.  The accurate statement
is: **Spec 176 established the secure data/lifecycle foundation; Spec 177 adds
request-scoped multi-view fusion and Provider-owned outputs, verified through
unit, CPU integration, functional worker, real MiniNDN, and claim-bounded
controlled-data gates.**

## Audit Matrix

| Dimension | Verdict | Evidence and boundary |
| --- | --- | --- |
| Intent fidelity | PASS | Requires a model whose terminal decision jointly consumes multiple views; independent detections are only a baseline. |
| Necessity / Occam | PASS | Existing code is single-evidence and digest-only; one additive multi-view layer is required. No second NDNSF protocol or mission lifecycle is introduced. |
| Architecture ownership | PASS | UAVs own image Data, coordinator owns job correlation, selected Provider owns fusion/output publication, NDNSF owns selection/security. |
| NDN data-centric fit | PASS at design | Requests carry exact signed Data references; images/results remain immutable producer-owned Data; no endpoint addressing or image bytes in service payloads. |
| Algorithm evidence | PASS for functional execution; scientific model evidence pending | MVCNN and RotationNet primary papers support multi-view recognition; Where2comm/CoPerception-UAV support the later collaborative-perception evaluation. The current worker executes the detector-guided pooling contract, while the registered checkpoint is unavailable and therefore cannot support an accuracy claim. |
| Security | PASS for the implemented boundary | Existing signer/digest/token/replay/provider-permission boundary is preserved; unit, Python, CPU integration, and MiniNDN paths exercise rejection and ownership checks without bypasses. |
| Migration safety | PASS | Current Spec 176 single-evidence path remains an explicit baseline; multi-view is additive. |
| Test rigor | PASS | Ordered G1-G6 gates are encoded; Spec177-specific G1-G6 pass. |
| Evidence integrity | PASS | Generated images are functional-only; quantitative claims require registered synchronized data and paired baselines. |
| Task executability | PASS | 13 cohesive tasks have exact files, dependencies, story ownership, and independent acceptance. |

## Findings and Remediation

| ID | Severity | Finding | Resolution |
| --- | --- | --- | --- |
| A1 | HIGH | A fixture criterion allowed a selectively chosen “difficult view” to make the multi-view result appear favorable. | Replaced it with deterministic consumed-view count and pooled-feature digest sensitivity to view removal/replacement. |
| A2 | HIGH | Listing `contributingViews` alone did not prove that the fusion layer used those views. | Added `fusionEvidence` containing operator, consumed-view count, and pooled-feature digest to requirements, data model, contract, and T005. |
| A3 | HIGH | “Recognition” could be read as unique vehicle-instance identification. | Scoped MVP output to vehicle-class recognition and defined `targetId` as correlation only. |
| A4 | MEDIUM | Generated images had no explicit multi-UAV producer mapping. | Added four logical producer identities to the fixture manifest while retaining the synthetic-data limitation. |
| A5 | MEDIUM | The model runtime dependency was written as unresolved “ONNX Runtime or PyTorch.” | Selected PyTorch for the MVP; ONNX export is explicitly later optimization. |
| A6 | MEDIUM | A multi-view refactor could silently break or reinterpret the Spec 176 single-evidence path. | Added FR-017 and regression ownership in T003/T004/T009/T013. |

## Residual Risks

1. **Checkpoint feasibility**: the registered profile currently uses the deterministic CPU adapter and a recorded model digest; a deployable vehicle checkpoint still requires a separate licensed artifact qualification before any accuracy claim.
2. **Synthetic identity drift**: image generation can alter vehicle geometry or details across views. This is acceptable only for the functional gate and is prohibited as accuracy evidence.
3. **Model-value evidence**: the generated fixture and current one-sample
   campaign prove execution and produce point estimates, not that multi-view
   improves accuracy.  A future claim requires a larger registered dataset and
   completed paired uncertainty analysis.
4. **Network cost**: transferring several images may dominate latency. G4/G5 must report bytes and stage timings rather than only model inference time.
5. **Calibrated extension**: Where2comm or other BEV fusion requires camera pose/synchronization and is deliberately outside the MVP.

## Implementation Start Gate

Proceed only in task order. T001-T010 and the controlled evaluator have now
passed. Future scientific work must first replace the one-sample synthetic
registration with a licensed, synchronized, calibrated multi-sample dataset;
it is not a blocker for the current functional Spec177 implementation.

## Coverage Metrics

- Functional requirements: 17/17 mapped to tasks
- Success criteria: 6/6 mapped to tasks
- User stories: 4/4 independently testable
- Planned tasks: 13; all match required checklist format
- Current completed Spec 177 tasks: 13/13
- Current single-view fixture qualification runs: 6/6 images detected as `Car`
- Current real multi-view model runs: 1 functional CPU adapter run (no accuracy claim)
- Current Spec 177 MiniNDN runs: five scenarios (two accepted, three explicit rejections), all gate-passing
