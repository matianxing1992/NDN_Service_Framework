# Feature Specification: UAV Multi-View Collaborative Recognition

**Feature Branch**: `UAV-Experimental`

**Created**: 2026-08-28

**Status**: Audited and ready for implementation

**Input**: Multiple UAVs capture the same target from different viewpoints and distances; a selected compute Provider must use all accepted views for collaborative target recognition and return verifiable fused results plus annotated images.

## User Scenarios & Testing

### User Story 1 - Recognize One Target From Multiple UAV Views (Priority: P1)

An incident coordinator opens one bounded recognition job for a target and supplies exact references to images captured by at least two distinct UAV identities. A capable Provider retrieves and verifies the referenced Data, combines the accepted views, and returns one fused recognition result whose provenance identifies every contributing view.

**Why this priority**: This is the research claim. Processing several images independently is not collaborative multi-view recognition unless the final decision demonstrably depends on multiple views.

**Independent Test**: Submit a six-view car fixture and verify that the result is produced from at least two distinct verified producers, reports the contributing view set, and emits fusion evidence whose consumed-view count and pooled-feature digest change when a non-duplicate contributing view is removed or replaced.

**Acceptance Scenarios**:

1. **Given** six valid views of the same vehicle from distinct viewpoints, **When** the coordinator requests multi-view recognition, **Then** the Provider returns one fused class decision, confidence, model provenance, and the exact names and digests of all accepted views.
2. **Given** one valid view and five unavailable or invalid views, **When** the job requires at least two views, **Then** the job fails explicitly as insufficient multi-view evidence and does not report a single-view result as multi-view recognition.
3. **Given** valid images belonging to two different targets, **When** their target or capture-window correlation is inconsistent, **Then** the Provider rejects the job before fusion.

---

### User Story 2 - Receive Verifiable Annotated Views (Priority: P2)

The coordinator receives a fused result manifest plus an annotated image corresponding to every contributing view. Each output is immutable, retrievable by exact name, and traceable to its source view and model execution.

**Why this priority**: Human operators need to inspect what the model recognized, while NDNSF needs named and signed result objects rather than opaque bytes hidden in a service response.

**Independent Test**: Fetch each output named by a result manifest, verify its signature and digest, and confirm that every accepted input view has exactly one corresponding annotation entry.

**Acceptance Scenarios**:

1. **Given** a successful fused recognition, **When** the coordinator fetches the result manifest, **Then** it can retrieve and verify one annotated image for every contributing input view.
2. **Given** a missing, corrupted, or wrongly signed annotation, **When** the coordinator validates the result, **Then** the job is marked incomplete and the invalid artifact is never displayed as trusted output.

---

### User Story 3 - Demonstrate Collaboration Without Physical Cameras (Priority: P3)

A developer can run the recognition pipeline using a versioned multi-view fixture of the same car at different viewpoints and distances. The fixture supports functional testing now, while a geometrically controlled dataset supports later quantitative evaluation.

**Why this priority**: The current team has no physical cameras, but implementation and end-to-end validation must not wait for hardware.

**Independent Test**: Reproduce the fixture hashes, execute the registered model locally and through NDNSF, and distinguish functional fixture evidence from scientific accuracy evidence.

**Acceptance Scenarios**:

1. **Given** the checked-in six-view fixture, **When** the registered algorithm test runs, **Then** all images are decoded, at least two distinct views are consumed, and a fused result plus annotations are produced.
2. **Given** generated images without trustworthy camera calibration, **When** results are summarized, **Then** they are labeled functional evidence and are excluded from accuracy, localization, or real-flight claims.
3. **Given** a controlled simulator or public collaborative-perception dataset with synchronized views and camera poses, **When** the evaluation campaign runs, **Then** multi-view accuracy and communication costs may be reported against registered single-view baselines.

---

### User Story 4 - Preserve NDNSF Security and Failure Semantics (Priority: P4)

The multi-view workflow preserves the existing request-scoped Provider selection, signed named Data verification, authorization, one-time tokens, replay protection, and single terminal ownership established by Spec 176.

**Why this priority**: Multi-view aggregation must not introduce a side channel that bypasses NDNSF's data or authorization boundaries.

**Independent Test**: Exercise invalid signer, digest mismatch, replay, mixed target, missing view, duplicate view, Provider timeout, and partial-output cases in unit, integration, and MiniNDN tests.

**Acceptance Scenarios**:

1. **Given** one view whose name, digest, or signer does not match its reference, **When** the Provider validates the job, **Then** that view is rejected and the job proceeds only if the registered minimum-view policy remains satisfied.
2. **Given** two Providers that can execute the job, **When** NDNSF selects one Provider, **Then** only the selected Provider owns and publishes the terminal fused result.

### Edge Cases

- Duplicate references to the same Data packet MUST count as one view.
- Multiple images from one UAV MAY be accepted, but a job configured for collaborative evidence MUST also satisfy its minimum distinct-producer count.
- Views outside the registered capture-time tolerance MUST be rejected rather than silently fused.
- A Provider MUST reject unsupported image encodings, view counts, model identifiers, or fusion profiles before inference.
- Late-arriving views MUST NOT mutate a completed immutable job; a new job attempt is required.
- The result MUST distinguish `insufficient-views`, `validation-failed`, `correlation-failed`, `inference-failed`, `annotation-publication-failed`, and `delivery-timeout`.
- If recognition succeeds but an annotated output cannot be published, the result MUST be incomplete rather than falsely successful.

## Requirements

### Functional Requirements

- **FR-001**: The system MUST represent one recognition attempt as a bounded job linked to an existing mission session, target identifier, capture window, and immutable set of view references.
- **FR-002**: Each view reference MUST include an exact Data name, producer identity, content digest, capture time, view identifier, and media description sufficient for deterministic validation.
- **FR-003**: A multi-view job MUST require at least two accepted views and MUST allow the caller to require at least two distinct producer identities.
- **FR-004**: The Provider MUST fetch and validate every candidate view at the existing signed-Data acceptance boundary before the view is eligible for fusion.
- **FR-005**: The Provider MUST reject duplicate, stale, cross-target, cross-window, unsupported, or unverifiable views with an explicit reason.
- **FR-006**: The final recognition decision MUST be generated by a registered multi-view algorithm that consumes the accepted view set jointly; independent per-view detections followed only by display-side concatenation do not satisfy this requirement.
- **FR-007**: The result manifest MUST include the fused vehicle-class decision, confidence, algorithm and model identifiers, model digest, contributing and rejected views, execution timestamps, terminal status, and fusion evidence containing the pooling operator, consumed-view count, and pooled-feature digest. The job's `targetId` is a correlation key, not a claim that the model performs unique vehicle-instance identification.
- **FR-008**: A successful job MUST publish one immutable annotated image reference for every contributing view and MUST bind each annotation to its source view.
- **FR-009**: Input images, annotated images, and result manifests MUST remain named Data under their respective producer namespaces; service payloads carry references and compact metadata rather than image bytes.
- **FR-010**: Only the NDNSF-selected Provider MUST own terminal execution and output publication for a job attempt.
- **FR-011**: The workflow MUST preserve authorization, token, replay-protection, signer, digest, and provider-permission checks from the current NDNSF runtime.
- **FR-012**: The system MUST expose stage-specific diagnostics for collection, fetch, validation, correlation, fusion, inference, annotation, publication, and delivery.
- **FR-013**: The checked-in generated fixture MUST be versioned with hashes, declared viewpoints and nominal distances, target identity, and a statement that it is functional rather than scientific evidence.
- **FR-014**: Quantitative algorithm claims MUST use a registered controlled or public multi-view dataset with synchronized views, ground truth, and camera metadata appropriate to the selected algorithm.
- **FR-015**: Evaluation MUST compare the same registered model family under one-view and multi-view inputs and report recognition quality, completion rate, latency, bytes transferred, accepted-view count, and failure stage.
- **FR-016**: Validation MUST follow the order unit tests, CPU integration tests, real model tests, MiniNDN multi-process tests, and only then simulator or cluster experiments.
- **FR-017**: The existing Spec 176 single-evidence detector path MUST remain available as an explicitly named single-view baseline and MUST retain its current validation behavior; multi-view recognition is additive and MUST NOT silently reinterpret single-view calls as multi-view success.

### Key Entities

- **MultiViewRecognitionJob**: One immutable request-scoped recognition attempt, including mission, target, time window, view policy, model requirement, and deadline.
- **ViewEvidenceReference**: Exact signed Data reference and capture metadata for one UAV image.
- **AcceptedViewSet**: De-duplicated, validated, time-correlated views eligible for joint inference.
- **MultiViewModelProfile**: Registered algorithm/model capabilities, supported view range, input media, calibration requirements, and model provenance.
- **FusedRecognitionResult**: Terminal vehicle-class decision and provenance that binds the result to the complete contributing view set.
- **AnnotatedViewReference**: Exact named Data reference for one Provider-produced annotated image tied to one source view.
- **FunctionalFixture**: Versioned generated images used for code-path validation without claims of real-world accuracy.
- **EvaluationDatasetRegistration**: Dataset identity, split, ground truth, synchronization, calibration, and license information required for quantitative claims.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The six-view car fixture completes one end-to-end job with all six images decoded, at least two distinct views jointly consumed, and six verifiable annotated outputs.
- **SC-002**: One-view, duplicate-view, mixed-target, stale-view, bad-digest, wrong-signer, and insufficient-view cases produce the registered terminal status in 100% of deterministic tests.
- **SC-003**: Repeating the same job input and model artifact yields identical contributing-view provenance and equivalent fused class output within the registered numerical tolerance.
- **SC-004**: The MiniNDN nominal case demonstrates multiple UAV producers, one selected compute Provider, exact-name Data retrieval, one terminal result owner, and no image bytes embedded in service request or response payloads.
- **SC-005**: On the registered quantitative dataset, the report includes paired one-view versus multi-view results over the same samples and does not claim a multi-view benefit unless the registered metric and uncertainty analysis support it.
- **SC-006**: Every published performance or accuracy number is traceable to a manifest containing source revision, model digest, dataset split, configuration, and result hashes.

## Assumptions

- Spec 176's mission-session lifecycle and bounded collaboration-job lifecycle remain authoritative and are extended rather than replaced.
- The first implementable algorithm is detector-guided multi-view feature aggregation for object recognition; calibrated collaborative 3D detection is a later extension.
- The generated fixture represents the same synthetic vehicle across views well enough for functional testing, but it does not supply trustworthy camera intrinsics, extrinsics, or real-world ground truth.
- Physical UAV cameras are not required for the first implementation gate.
- Model training is outside the first MVP unless a compatible pretrained checkpoint cannot satisfy the registered functional fixture.

## Out of Scope

- Claiming real-flight accuracy from generated images.
- Full online training, federated learning, or continual learning aboard UAVs.
- Multi-target tracking across long video sequences.
- Replacing NDNSF's existing Provider selection, security, or mission lifecycle.
- Treating a collection of independent single-view labels as a fused multi-view result.
