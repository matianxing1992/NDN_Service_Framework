# Contract: MVCNN ONNX CPU Model and Execution Evidence

## Model Registration

```json
{
  "schema": "ndnsf-uav-mvcnn-onnx-profile/v1",
  "profileId": "vehicle-mvcnn-v1",
  "algorithmId": "mvcnn-onnx-maxpool/v1",
  "artifact": {
    "name": "NDNSF-UAV-APP/models/mvcnn_vehicle_cpu.onnx",
    "sha256": "sha256:14ec256fbc84e1c6d9d0cf593ca47ce151c641c5b3280535ca4dcbedd1a4c317",
    "sourceRepository": "NDNSF-UAV-APP/tools/mvcnn_model.py",
    "sourceRevision": "local-source:sha256:4a8610c47ed89159293cf5b36939360ca257b9cf391b0691add652d45ce05a76",
    "checkpointSha256": "sha256:6acdbf2c34c7a2cd2cc6f7c76d3ced2fcd4e99f71e3907131dbbc113bb25b279",
    "license": "MIT"
  },
  "runtime": {
    "requiredProvider": "CPUExecutionProvider",
    "qualifiedVersion": "1.19.2",
    "fallbackAllowed": false
  },
  "inputs": {
    "images": {"dtype": "float32", "shape": [1, 6, 3, 224, 224]},
    "viewMask": {"dtype": "bool", "shape": [1, 6]}
  },
  "outputs": {
    "logits": {"dtype": "float32", "shape": [1, "classCount"]},
    "pooledFeatures": {"dtype": "float32", "shape": [1, "featureCount"]}
  },
  "viewPolicy": {"operationalMinimum": 2, "maximum": 6, "baseline": 1},
  "preprocessingProfile": "rgb-resize-224-center-crop/v1",
  "classMap": ["car", "truck", "person"],
  "numericalTolerance": {"absolute": 0.00001, "relative": 0.0001}
}
```

The active repository profile is `vehicle-mvcnn-v1`; all artifact and runtime
values above are frozen. Runtime output digests and timings are populated by
each execution evidence record rather than being guessed in this contract.

## Input Admission Contract

The model worker receives only locally materialized views that have already crossed the NDNSF exact-name, signature, digest, authorization, target, and capture-window boundary. The worker still verifies that the manifest is complete and consistent before tensor construction.

```json
{
  "schema": "ndnsf-uav-mvcnn-input/v1",
  "missionSessionId": "mission-001",
  "jobId": "recognition-001",
  "attempt": 1,
  "modelProfileId": "vehicle-mvcnn-v1",
  "views": [
    {
      "slot": 0,
      "viewId": "front-left",
      "exactDataName": "/uav/a/.../FRAME/1",
      "contentDigest": "sha256:<verified-view-digest>",
      "producerIdentity": "/uav/a",
      "materializedPath": "<verified-local-path>"
    }
  ]
}
```

The model input contains no network endpoint and does not change ownership of source Data.

## Runtime Acceptance

A real-model result is admissible only when all of the following hold:

1. artifact bytes match the active profile digest;
2. ONNX graph checker and exact I/O metadata pass;
3. active session providers equal the registered CPU-only subject;
4. fallback is disabled and `fallbackUsed=false`;
5. the mask contains exactly the accepted-view count;
6. logits and pooled features are finite, non-empty, correctly shaped outputs;
7. class index resolves through the registered class map;
8. execution evidence binds the terminal Provider and input view references.

## Result Evidence Extension

The existing Spec 177 result remains authoritative and adds:

```json
{
  "modelExecution": {
    "profileId": "vehicle-mvcnn-v1",
    "artifactDigest": "sha256:14ec256fbc84e1c6d9d0cf593ca47ce151c641c5b3280535ca4dcbedd1a4c317",
    "runtimeVersion": "1.19.2",
    "activeProviders": ["CPUExecutionProvider"],
    "acceptedViewCount": 6,
    "viewMaskDigest": "sha256:<runtime-mask-digest>",
    "inputTensorDigest": "sha256:<runtime-input-tensor-digest>",
    "logitsDigest": "sha256:<runtime-logits-digest>",
    "pooledRepresentationDigest": "sha256:<runtime-pooled-feature-digest>",
    "sessionCreateMs": 0.0,
    "inferenceMs": 0.0,
    "peakRssBytes": 0,
    "fallbackUsed": false,
    "terminalOwner": "/provider/cpu"
  }
}
```

## Terminal Failure Vocabulary

- `model-artifact-missing`
- `model-artifact-digest-mismatch`
- `model-license-unverified`
- `model-contract-incompatible`
- `model-provider-mismatch`
- `model-input-invalid`
- `model-output-invalid`
- `model-inference-timeout`
- `model-inference-failed`

These extend, but do not collapse, the existing network, view-validation, correlation, annotation-publication, and delivery statuses.
