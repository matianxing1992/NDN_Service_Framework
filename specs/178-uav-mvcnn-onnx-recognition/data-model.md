# Data Model: UAV MVCNN ONNX Joint Recognition

## MvcnnOnnxModelProfile

Immutable registration for one qualified real-model subject.

| Field | Meaning | Validation |
| --- | --- | --- |
| `profileId` | Stable application-visible profile name | Non-empty and versioned |
| `algorithmId` | MVCNN-family architecture identifier | Must denote joint view pooling |
| `artifactName` | Content-addressed artifact filename | `.onnx`; no timestamp-only identity |
| `artifactDigest` | SHA-256 of exact model bytes | Recomputed before session creation |
| `sourceRepository` / `sourceRevision` | Model/export provenance | Immutable revision required |
| `checkpointDigest` | Native checkpoint used for export | Required when graph is exported locally |
| `license` / `weightTerms` | Code and weight permissions | Both explicitly recorded |
| `opset` / `irVersion` | ONNX compatibility | Must match inspected graph |
| `inputContract` | Names, types, shapes, layout | Exact match before inference |
| `outputContract` | Logits and pooled-feature names/types/shapes | Both outputs required |
| `maximumViews` | Bounded view capacity | Exactly 6 for the first profile |
| `maskSemantics` | Valid versus padded slot behavior | Masked slots cannot affect outputs |
| `preprocessingProfile` | Resize, crop, normalization, channel order | Digest/version required |
| `classMap` | Logit index to semantic class | Digest/version required |
| `executionProvider` | Qualified runtime subject | Exactly `CPUExecutionProvider` |
| `runtimeVersion` | Qualified ONNX Runtime version | Recorded from live session |
| `numericalTolerance` | Native/ONNX and repeat tolerance | Finite and pre-registered |

State: `proposed -> provenance-verified -> native-qualified -> onnx-qualified -> active`, with any failed gate transitioning to `rejected`.

## MvcnnInputBatch

Deterministic representation of one accepted job view set.

| Field | Meaning |
| --- | --- |
| `jobId` / `attempt` | Request-scoped execution identity |
| `orderedViewReferences` | Exact admitted Data references in canonical slot order |
| `imagesTensor` | Preprocessed bounded image tensor |
| `viewMask` | Validity indicator for every bounded slot |
| `tensorDigest` / `maskDigest` | Content evidence before model execution |
| `preprocessingProfileDigest` | Exact transformation identity |
| `acceptedViewCount` | Number of true, unmasked views |
| `paddedViewCount` | Number of excluded slots |

Validation requires 1-6 unique admitted views for evaluation, 2-6 for operational multi-view jobs, one mask bit per slot, canonical slot mapping, and zero influence from masked slots.

## MvcnnExecutionEvidence

Evidence produced by one real ONNX inference.

| Field | Meaning |
| --- | --- |
| `profileId` / `artifactDigest` | Exact registered model subject |
| `runtimeVersion` / `activeProviders` | Live execution environment |
| `sessionOptionsDigest` | Threading, optimization, and determinism settings |
| `inputNamesShapesTypes` | Runtime-observed input contract |
| `outputNamesShapesTypes` | Runtime-observed output contract |
| `acceptedViewCount` / `viewMaskDigest` | Joint-input evidence |
| `logitsDigest` | Digest of raw fused logits |
| `pooledRepresentationDigest` | Digest of joint pooled features |
| `predictedClass` / `confidence` | Interpreted model output |
| `sessionCreateMs` / `inferenceMs` / `peakRssBytes` | CPU cost evidence |
| `terminalOwner` | Selected Provider identity |
| `fallbackUsed` | Must be `false` for real-model evidence |

## PairedViewTreatment

Pre-registered view-count treatment for one target.

- `datasetId`, `split`, `targetId`, and ground truth
- deterministic eligible view list and subset rule
- treatment view count in `{1, 2, 4, 6}`
- exact selected view identifiers and digests
- shared model/preprocessing/host/deadline configuration digests

## PairedRecognitionObservation

One immutable result row.

- treatment identity and source hashes
- completion and failure stage
- predicted class, correctness, confidence, logits digest, pooled-feature digest
- model-only and end-to-end latency
- bytes fetched/published, accepted views, and peak RSS
- exact output artifact references and hashes

No row may be deleted after outcome inspection. Invalid or unavailable targets remain explicit exclusions with registered reasons.

## ClaimBoundary

Machine-readable evidence category:

- `functional-only`: generated fixture or deterministic adapter; no accuracy claim
- `export-qualified`: native and ONNX outputs agree on the artifact's native data
- `quantitative-inconclusive`: paired campaign complete but uncertainty does not support a benefit
- `quantitative-supported`: pre-registered effect and uncertainty support the stated bounded claim

## Execution State Transitions

```text
JOB_ACCEPTED
  -> VIEWS_FETCHED
  -> VIEWS_VALIDATED
  -> MODEL_PROFILE_VERIFIED
  -> INPUT_BATCH_MATERIALIZED
  -> CPU_SESSION_VERIFIED
  -> JOINT_INFERENCE_COMPLETED
  -> ANNOTATIONS_PUBLISHED
  -> RESULT_PUBLISHED
  -> TERMINAL_ACCEPTED
```

Every arrow has an explicit terminal failure counterpart; network, validation, model, annotation, and delivery failures remain distinct.
