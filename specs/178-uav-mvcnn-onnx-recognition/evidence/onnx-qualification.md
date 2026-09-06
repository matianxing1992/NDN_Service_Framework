# T002/T003 ONNX qualification

The exported artifact is `NDNSF-UAV-APP/models/mvcnn_vehicle_cpu.onnx` with
digest `sha256:14ec256fbc84e1c6d9d0cf593ca47ce151c641c5b3280535ca4dcbedd1a4c317`.
The graph uses opset 17 and exposes exactly:

```text
images    float32 [1, 6, 3, 224, 224]
viewMask  bool    [1, 6]
logits           [1, 3]
pooledFeatures   [1, 32]
```

Qualification checks passed with ONNX Runtime `1.19.2` and the explicit
provider list `[CPUExecutionProvider]`:

- ONNX checker and exact I/O names/shapes/types
- masked-slot neutrality
- valid-view permutation equivalence
- deterministic repeated inference
- real 2/4/6-view fixture execution with one joint result and one annotation
  per accepted view
- missing/incorrect model digest fails closed
- one-view execution is rejected unless `--paired-baseline` is explicit

The real worker emits model digest, checkpoint digest, runtime/provider list,
input contract, mask/logits/pooled-feature digests, predicted class,
confidence, CPU inference time, accepted-view count, and terminal owner. It has
no detector, voting, download, or automatic fallback path.

