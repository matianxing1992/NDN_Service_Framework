# Data Model: UAV Multi-View Collaborative Recognition

## MultiViewRecognitionJob

Fields:

- `missionSessionId`: existing Spec 176 mission session.
- `jobId`: unique request-scoped identifier.
- `attempt`: immutable retry/attempt number.
- `targetId`: correlation identity for the observed target.
- `captureWindowStartMs`, `captureWindowEndMs`: allowed capture interval.
- `views[]`: ordered for transport but semantically unordered view references.
- `minimumViews`: at least 2 for multi-view success.
- `minimumDistinctProducers`: default 2 for collaborative recognition.
- `modelProfileId`: required registered model profile.
- `deadlineMs`: global terminal deadline.

Validation:

- job identity, attempt, target, and window are non-empty and immutable;
- exact Data names and `(producer, viewId, digest)` tuples are unique;
- all views identify the same target and capture window;
- requested view count is within the selected model profile's supported range.

State:

```text
CREATED -> REQUESTED -> PROVIDER_SELECTED -> COLLECTING -> VALIDATING
        -> FUSING -> ANNOTATING -> PUBLISHING -> COMPLETED

Any non-terminal state -> FAILED | TIMED_OUT | CANCELLED
```

## ViewEvidenceReference

Fields: `viewId`, `producerIdentity`, `exactDataName`, `contentDigest`, `captureTimeMs`, `targetId`, media description, optional viewpoint label, and optional camera pose/intrinsics. Camera calibration is optional for the MVP profile and mandatory for calibrated profiles.

## AcceptedView

Adds verified signer identity, fetched content or bounded local materialization, validation flags/time, and per-view detection/crop metadata to a valid reference.

## MultiViewModelProfile

Fields: profile/algorithm/model identifiers and digest, supported media and image bounds, view-count policy, distinct-producer policy, calibration requirement, pooling/fusion operator, preprocessing profile, device requirements, and output schema version.

## FusedRecognitionResult

Fields: job identity and status, fused label/confidence, accepted and rejected views, algorithm/model/preprocessing provenance, fusion operator, consumed-view count, pooled-feature digest, stage timings and byte counts, annotated output references, and result-manifest name/digest.

Invariant: a successful multi-view result lists at least the registered minimum unique views, and its fusion evidence changes when a non-duplicate contributing view is removed or replaced under the deterministic functional profile.

## AnnotatedViewReference

Fields: source view ID/digest, Provider-owned exact Data name/digest, media description, fused label/confidence, per-view boxes, Provider signer, and model digest.

## FunctionalFixtureRegistration

Fields: fixture/target identity, image paths and hashes, nominal view labels/distances, generation date, limitations, and `scientificAccuracyClaimAllowed=false`.

## EvaluationDatasetRegistration

Fields: dataset name/version/license/source, immutable split/sample list, synchronization/calibration metadata, ground-truth type, one-view selection and N-view grouping rules, model/checkpoint/preprocessing digests, primary metric, and uncertainty method.
