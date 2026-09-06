# YOLO numerical component evidence v1

Owner: T010/T013; implements FR-011 and the numerical portion of FR-019.
This is a component contract, not a replacement terminal CandidateRecord.

## Subject and input

The maintained ACK-driven User loads the registered fixed P3 fixture
`tests/fixtures/spec180/yolo26n/fixed-fixture.ppm` and verifies its SHA-256,
revision and preprocessing against the canonical package manifest. Only the
registered 640x640 float32 RGB/NCHW [0,1] input is accepted. Resize uses bilinear
half-pixel centers with edge clamping (`align_corners=False` semantics), without
Torch or a local model forward pass in the deployed path. Float32 rounding may
differ from the offline Torch implementation; the focused fixture comparison
allows absolute error 2e-7, not bitwise equality or inference equivalence.

The published, encrypted REPO_REF contains one native tensor named `images`.
`--native-tensor-input` is mandatory in this qualification path. A supplied
encoded input file is accepted only when byte-identical to that registered
payload. This does not change the separate, explicit legacy/offline examples.

## Reference and comparison

The reference is `oracle/full-model-output.npy` within the canonical package.
Before input publication, verify its file digest, declared dtype/shape,
fixture identity and canonical rows against the package manifest. Disable
NumPy pickle loading and reject paths escaping the owning package/repository.
Manifest/oracle hashes are component byte binding, not independent signature
authority; the full candidate gate must bind the manifest and oracle files.

The received response must be a native bundle containing exactly one
`predictions` float32 tensor of shape [1,N,6]. Reject duplicate/empty tensor
names, negative dimensions, trailing/truncated bytes, NaN/Inf, invalid
confidence, and non-integral/out-of-range COCO classes. Rows are
`[x1,y1,x2,y2,confidence,class]`. Apply the registered confidence >=0.001
filter and confidence-desc/class-asc/xyxy-asc canonical ordering to the actual
output. The stored oracle must already have this canonical form.

Require identical post-filter shapes and class IDs, then compare against the
reference with `abs(actual - expected) <= 1e-3 + 1e-4 * abs(expected)`.
No rows are omitted to reconcile a shape mismatch. Comparison mismatch or
malformed response cannot produce the successful User marker.

## Record and failure behavior

After a successful protocol response, User exclusively creates
`<lifecycle-output-dir>/yolo-numerical.json`; existing files are never replaced.
The record has `schemaVersion=spec180-yolo-numerical-v1`, case, requestId,
attemptId, planDigest, manifestDigest, oracleDigest, fixtureDigest,
inputTensorDigest (contiguous float32 NCHW bytes), and responseDigest (received
bundle bytes). Successful decoding adds matched, shape, expectedShape, atol,
rtol and maxAbsError (float64 arithmetic; null for shape mismatch). Invalid
decoding/numerical values record matched=false and the controlled reason
`INVALID_NUMERICAL_RESPONSE`. No plaintext tensors, detections or remote error
messages are included. Failed remote protocol responses exit 3 without a
numerical record; numerical failure exits 4 without a successful User marker.

This artifact does not establish CUDA EP activation, physical GPU identity,
network protocol validity, process cleanup, or candidate identity. The pending
T013 collector must bind this record to the actual lifecycle, native execution
records, observed supervision and immutable candidate before constructing and
validating `spec180-result.json`. Until that producer exists, the supervisor
must continue failing on a missing terminal result.
