# Feature Specification: UAV MVCNN ONNX Joint Recognition

**Feature Branch**: `UAV-Experimental`

**Created**: 2026-08-29

**Status**: Implemented — claim-bounded qualification

**Input**: Replace the Spec 177 functional multi-view fallback with a real MVCNN model artifact executed on CPU through ONNX Runtime, so that multiple UAV viewpoints are jointly consumed by one recognition inference and compared fairly with the same model under a single-view input.

## User Scenarios & Testing

### User Story 1 - Jointly Recognize a Target With a Real MVCNN Artifact (Priority: P1)

An incident coordinator submits exact references to two through six verified UAV images of the same target. The selected compute Provider loads one registered MVCNN artifact, jointly consumes the accepted view set in one model inference, and returns one fused vehicle-class decision with evidence that the model used the complete view set.

**Why this priority**: Spec 177 proved the multi-view lifecycle and fusion contract with a CPU-safe functional adapter. The next research step must replace the fallback with an immutable, reproducible model artifact whose inference genuinely combines multiple views.

**Independent Test**: Run the same registered artifact with 2, 4, and 6 valid views. Verify that each run uses the CPU inference provider, reports the accepted model-input view count, produces fused logits and a pooled-representation digest, and never substitutes the deterministic fallback.

**Acceptance Scenarios**:

1. **Given** six verified views of one vehicle, **When** the selected Provider executes the registered MVCNN artifact, **Then** one inference consumes all six views and returns one fused label, confidence, logits digest, pooled-representation digest, model digest, and exact contributing-view provenance.
2. **Given** the same views in a different order, **When** the registered symmetric view-fusion profile executes, **Then** the class output and logits remain equivalent within the registered tolerance.
3. **Given** a missing, altered, unlicensed, or shape-incompatible model artifact, **When** inference is requested, **Then** the Provider fails explicitly before model execution and does not fall back to a fake or per-view-only result.

---

### User Story 2 - Preserve NDNSF Data and Security Boundaries (Priority: P2)

The MVCNN execution remains inside the existing Spec 176/177 job lifecycle. UAV images stay immutable signed named Data, the service request carries references rather than image bytes, and only the NDNSF-selected Provider can publish the accepted result and annotations.

**Why this priority**: Introducing a real model runtime must not bypass exact-name retrieval, signer/digest validation, authorization, replay protection, or terminal ownership.

**Independent Test**: Execute valid and invalid jobs through the current CPU integration and MiniNDN paths and verify exact-name fetches, model-input admission, one selected terminal owner, and fail-closed behavior for invalid data or model provenance.

**Acceptance Scenarios**:

1. **Given** one view whose signer, digest, target, or capture window is invalid, **When** the Provider constructs the MVCNN input, **Then** the view is excluded before inference and the job proceeds only if the registered minimum-view policy is still satisfied.
2. **Given** two eligible Providers, **When** NDNSF selects one Provider, **Then** only that Provider loads the model, executes the accepted attempt, and publishes the terminal result.
3. **Given** a replayed job or consumed one-time token, **When** execution is attempted, **Then** the existing NDNSF rejection occurs before model inference.

---

### User Story 3 - Compare Single-View and Multi-View Recognition Fairly (Priority: P3)

A researcher evaluates the same model family, preprocessing, class map, target samples, and CPU host under one-view and 2/4/6-view inputs. The report shows whether additional viewpoints improve recognition and separately reports their latency and communication costs.

**Why this priority**: A multi-view implementation is not evidence of an advantage. A defensible claim requires a paired comparison in which view count is the intended treatment and all other relevant factors remain controlled.

**Independent Test**: Register a synchronized vehicle-oriented multi-view dataset, create deterministic 1/2/4/6-view subsets for every eligible target, run paired inference, and verify complete per-sample results, uncertainty, model/dataset provenance, and claim boundaries.

**Acceptance Scenarios**:

1. **Given** a registered target sample with at least six valid views, **When** the evaluation runs, **Then** it emits paired one-view, two-view, four-view, and six-view rows using the same model artifact and preprocessing profile.
2. **Given** paired results, **When** the report is generated, **Then** it includes recognition quality, completion rate, inference and end-to-end latency, transferred bytes, accepted-view count, peak memory, failure stages, effect size, and uncertainty.
3. **Given** results whose uncertainty does not support an improvement, **When** conclusions are produced, **Then** the report states that no multi-view benefit was demonstrated rather than selecting favorable samples or seeds.

---

### User Story 4 - Retain Functional and Single-View Baselines (Priority: P4)

A developer can still run the deterministic Spec 177 adapter for contract tests and the Spec 176 single-evidence detector path for regression testing, but every output states which execution path produced it.

**Why this priority**: Test doubles are valuable for fast unit/integration gates, but silently using them in a real-model campaign would invalidate the evidence.

**Independent Test**: Exercise the real MVCNN, deterministic adapter, and single-view baseline independently and verify that their profile identifiers, evidence, and allowed claims cannot be confused.

**Acceptance Scenarios**:

1. **Given** a real-model campaign, **When** the ONNX artifact cannot execute, **Then** the campaign fails and cannot continue under the deterministic adapter.
2. **Given** a contract-only test, **When** the deterministic adapter runs, **Then** the output is labeled functional-only and cannot enter the real-model accuracy table.

### Edge Cases

- Duplicate exact Data references MUST count as one view and MUST NOT occupy multiple model-input slots.
- The registered graph MUST define how one-view and 2/4/6-view inputs are represented; padding MUST be masked and MUST NOT influence pooling or logits.
- Unsupported view counts, tensor dimensions, element types, input/output names, class maps, or model opsets MUST fail before inference.
- The CPU qualification path MUST reject an unexpected CUDA, TensorRT, or other accelerator provider rather than silently changing the execution subject.
- A NaN/Inf output, missing logits, invalid class index, or missing pooled representation MUST produce an explicit inference failure.
- View-order permutation MUST remain equivalent within the registered numerical tolerance for a symmetric fusion profile.
- Removing or replacing a non-duplicate contributing view SHOULD change the pooled representation in at least one registered sensitivity case; otherwise the model-use claim requires investigation.
- A deadline expiration while fetching views MUST NOT be reported as model inference failure, and a model failure MUST NOT be reported as a network timeout.
- Optional per-view target cropping MAY be used, but it MUST be frozen and identical across one-view and multi-view treatments; independent YOLO labels or majority voting do not satisfy joint MVCNN inference.
- Generated car images MAY validate the execution path but MUST NOT support a recognition-accuracy or real-flight claim.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST extend the existing Spec 177 multi-view job and result contract rather than introduce a second mission or collaboration lifecycle.
- **FR-002**: A real-model job MUST jointly submit two through six accepted views to one registered MVCNN inference; a one-view execution MUST be available only as an explicitly named paired baseline.
- **FR-003**: The registered model profile MUST bind the exact artifact digest, license, source revision, model family, class map, input/output contract, supported view policy, preprocessing profile, numerical tolerance, and expected CPU execution provider.
- **FR-004**: The qualification subject MUST be a real ONNX model artifact executed through ONNX Runtime `CPUExecutionProvider`; missing or incompatible artifacts, or activation of any other execution provider, MUST fail closed without downloading a replacement or using the deterministic adapter.
- **FR-005**: The model input contract MUST represent up to six views plus an explicit validity mask, or an equivalently auditable bounded-view representation, so unused slots cannot affect fusion.
- **FR-006**: The final decision MUST be produced from a joint multi-view representation inside the registered inference subject; independent per-view detections, label voting, or display-side aggregation alone do not satisfy this requirement.
- **FR-007**: Model-input construction MUST be deterministic and MUST preserve the exact mapping from accepted view references to model-input slots, including preprocessing and mask evidence.
- **FR-008**: The Provider MUST validate model provenance and every accepted view's name, signer, digest, target, time window, media type, and duplicate status before starting inference.
- **FR-009**: Real-model execution evidence MUST include the model/profile digest, runtime and execution-provider identity, input view count, input tensor contract, view-mask digest, logits digest, pooled-representation digest, predicted class, confidence, CPU inference time, and terminal owner.
- **FR-010**: The result MUST retain exact contributing/rejected view provenance and one Provider-owned annotated output reference per contributing view, consistent with Spec 177.
- **FR-011**: Images and result artifacts MUST remain signed named Data under their producer namespaces; NDNSF request/response payloads MUST carry references and compact metadata rather than image bytes.
- **FR-012**: Only the NDNSF-selected Provider MUST execute and publish the terminal model result, while authorization, one-time token, replay, signer, digest, and provider-permission checks remain enforced.
- **FR-013**: The deterministic Spec 177 adapter MUST remain available for unit and CPU contract tests under a distinct functional-only profile and MUST never be an automatic real-model fallback.
- **FR-014**: The Spec 176 single-evidence detector path MUST remain available as a distinct one-view regression baseline without being relabeled as MVCNN.
- **FR-015**: The generated six-view car fixture MUST be usable as a real-artifact smoke test while remaining excluded from scientific accuracy claims.
- **FR-016**: Quantitative evaluation MUST use a registered, licensed, synchronized, vehicle-oriented multi-view dataset with ground truth, deterministic target grouping, and enough independent samples for the pre-registered uncertainty analysis.
- **FR-017**: Evaluation MUST compare paired 1/2/4/6-view inputs using the same artifact, class map, preprocessing, target samples, CPU reference host, deadline, and measurement definitions.
- **FR-018**: Evaluation MUST report per-view-count recognition quality, completion rate, CPU inference and end-to-end latency, bytes transferred, accepted-view count, peak memory, failure stage, paired effect size, and uncertainty.
- **FR-019**: A multi-view benefit MUST NOT be claimed unless the pre-registered paired analysis supports it; all negative or inconclusive results MUST remain visible.
- **FR-020**: Validation MUST proceed in the order model-contract unit tests, real ONNX CPU smoke, CPU integration, MiniNDN multi-process execution, and only then the registered paired campaign.
- **FR-021**: MiniNDN evidence MUST distinguish network retrieval, input validation, ONNX session creation, joint inference, annotation publication, and terminal delivery stages.
- **FR-022**: Every retained result MUST be traceable to source revision, model/dataset/configuration digests, exact command, runtime versions, reference-host identity, and output hashes.

### Key Entities

- **MvcnnOnnxModelProfile**: Immutable registration for the actual model artifact, license, class map, tensor interface, view/mask policy, preprocessing, CPU provider requirement, and numerical tolerances.
- **MvcnnInputBatch**: Deterministic tensor and mask representation of one accepted view set, bound to exact view references and preprocessing evidence.
- **MvcnnExecutionEvidence**: Runtime identity, CPU provider selection, input/output contracts, view count, timing, logits and pooled-representation digests, and terminal owner for one inference.
- **PairedViewTreatment**: One target sample evaluated under the registered 1/2/4/6-view subsets with every other controlled factor held constant.
- **PairedRecognitionObservation**: Per-target outcome containing prediction, correctness, confidence, completion, latency, bytes, memory, failure stage, and artifact hashes.
- **ClaimBoundary**: Machine-readable statement identifying functional-only, quantitative-but-inconclusive, or statistically supported evidence.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The registered ONNX artifact completes one-view and 2/4/6-view CPU runs with the expected input/output contract and reports ONNX Runtime `CPUExecutionProvider` as the only active execution provider in 100% of qualification runs.
- **SC-002**: A six-view job reports six accepted model inputs, one valid mask, one joint pooled representation, one fused decision, and six verifiable annotations without invoking the deterministic fallback.
- **SC-003**: Repeating the same input three times yields the same class and logits within the registered tolerance; permuting valid views yields an equivalent result for the symmetric fusion profile.
- **SC-004**: Missing/wrong model digest, unsupported view count, malformed tensor, invalid view, wrong provider, NaN/Inf output, replay, and non-selected Provider cases reach their registered failure stage in 100% of deterministic tests.
- **SC-005**: The real MiniNDN nominal case demonstrates multiple UAV Data producers, one selected CPU Provider, verified image retrieval, an actual ONNX inference event, one terminal result owner, and no image bytes in service payloads.
- **SC-006**: Every eligible registered dataset target has complete paired 1/2/4/6-view observations or an explicit failure record; no target is silently removed after results are known.
- **SC-007**: The paired report includes point estimates, effect sizes, confidence intervals, sample count, exclusions, latency, bytes, and memory for every treatment and labels unsupported benefit claims as inconclusive.
- **SC-008**: At least 95% of qualified functional jobs complete within the registered five-second global deadline on the reference CPU host, or the evidence records that the model cannot meet the bound without redefining the subject.
- **SC-009**: Existing Spec 176 single-view and Spec 177 deterministic multi-view regression gates retain their prior behavior and remain clearly separated from the real-model evidence.

## Assumptions

- Spec 176 and Spec 177 remain the authoritative lifecycle, named-Data, security, and output-provenance foundation.
- The first Spec 178 subject is one legally usable pretrained or fine-tuned MVCNN-family artifact exported to ONNX and qualified on a CPU reference host.
- A single artifact with a bounded view dimension and mask is preferred so the same learned parameters are used for 1/2/4/6-view comparisons.
- The qualified artifact and checkpoint are now present in the repository with frozen hashes; future changes must repeat the provenance and export gates before replacing them.
- Standard MVCNN checkpoints trained on rendered 3D objects may not generalize to UAV vehicle imagery; functional execution and recognition quality are separate questions.
- Optional detector/crop preprocessing is fixed before paired evaluation and is not counted as multi-view fusion.
- Model training from scratch is not required unless no compatible and legally usable artifact can satisfy the vehicle-oriented task.

## Out of Scope

- GPU, TensorRT, CUDA, or mixed CPU/GPU performance claims.
- Calling independent YOLO detections or majority voting a joint MVCNN result.
- Training a state-of-the-art collaborative 3D detector or BEV fusion system.
- Real-flight accuracy claims from the generated fixture.
- Changing NDNSF Provider selection, security, token, permission, or mission-session semantics.
- Hiding an unavailable or failed real model behind a deterministic fallback.
