# Contract: Multi-View Recognition Job and Result

This is the semantic contract. Concrete C++/Python encoding is chosen during implementation without changing these invariants.

## Request payload

```json
{
  "schema": "ndnsf-uav-multiview-job/v1",
  "missionSessionId": "mission-001",
  "jobId": "recognition-001",
  "attempt": 1,
  "targetId": "synthetic-blue-suv-001",
  "captureWindow": {"startMs": 0, "endMs": 1000},
  "minimumViews": 2,
  "minimumDistinctProducers": 2,
  "modelProfileId": "vehicle-mvcnn-v1",
  "views": [
    {
      "viewId": "view-01",
      "producerIdentity": "/uav/A",
      "exactDataName": "/uav/A/UAV/IMAGE/mission-001/view-01/v=1/seg=0",
      "contentDigest": "sha256:...",
      "captureTimeMs": 100,
      "targetId": "synthetic-blue-suv-001",
      "mediaType": "image/png"
    }
  ]
}
```

The payload MUST NOT contain image bytes.

## Provider capability metadata

```json
{
  "modelProfileId": "vehicle-mvcnn-v1",
  "algorithmId": "detector-guided-mvcnn-pooling/v1",
  "modelDigest": "sha256:...",
  "minimumViews": 2,
  "maximumViews": 6,
  "requiresCalibration": false,
  "ready": true,
  "queueDepth": 0,
  "deviceClass": "cuda"
}
```

## Result payload

```json
{
  "schema": "ndnsf-uav-multiview-result/v1",
  "missionSessionId": "mission-001",
  "jobId": "recognition-001",
  "attempt": 1,
  "status": "completed",
  "fusedDecision": {"label": "car", "confidence": 0.97},
  "model": {
    "profileId": "vehicle-mvcnn-v1",
    "algorithmId": "detector-guided-mvcnn-pooling/v1",
    "modelDigest": "sha256:..."
  },
  "contributingViews": ["view-01", "view-02"],
  "rejectedViews": [],
  "fusionEvidence": {
    "operator": "elementwise-max/v1",
    "consumedViewCount": 2,
    "pooledFeatureDigest": "sha256:..."
  },
  "resultManifest": {"exactDataName": "/provider/...", "contentDigest": "sha256:..."},
  "annotatedViews": [
    {
      "viewId": "view-01",
      "sourceDigest": "sha256:...",
      "exactDataName": "/provider/.../ANNOTATION/view-01/v=1/seg=0",
      "contentDigest": "sha256:..."
    }
  ]
}
```

## Terminal status vocabulary

- `completed`
- `insufficient-views`
- `validation-failed`
- `correlation-failed`
- `unsupported-model-profile`
- `inference-failed`
- `annotation-publication-failed`
- `delivery-timeout`
- `cancelled`

## Acceptance invariants

- `completed` requires at least the registered minimum unique views.
- The contributing set matches the fusion adapter's consumed-view set.
- The fusion evidence binds the pooling operator, consumed-view count, and pooled-feature digest to the model execution.
- Every contributing view has exactly one annotated output reference.
- All exact names, digests, and signer identities verify before terminal acceptance.
- Only the selected Provider may publish the accepted terminal result for the job attempt.
