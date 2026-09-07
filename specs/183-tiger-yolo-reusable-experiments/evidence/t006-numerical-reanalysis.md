# T006 numerical reanalysis component

2026-09-07. PARTIAL / model, native runtime, SIF and Tiger execution NOT_RUN.

The maintained YOLO User can now explicitly retain the authorized benchmark
Response as `yolo-response.bin`: exclusive creation, mode0600, fsync, at most
1 MiB, with relative path/byte count/digest in `yolo-numerical.json`. This is
opt-in via `--retain-numerical-response` and remains off for ordinary apps.
Only the User output mount contains it; never mount it into a Provider, log
its contents or commit it. A partial write/old record cannot become a new PASS.

The Spec183 request launcher enables this evidence mode and supplies candidate
ID/digest from the prepared offer trust record. It now requires the prepared
Worker/plan binding and rejects malformed or inconsistent candidate fields
before launching a User. Previously these environment fields were absent,
leaving numerical evidence without candidate provenance.

`runtime/yolo_result.reanalyze_numerical_response` reads the retained bytes,
binds case/request/attempt/plan/candidate, reference/fixture/input digests and
terminal response digest, and recomputes the existing fixed-reference NumPy
comparison. It requires exactly the native predictions tensor and exact
reported comparison/tolerances, not merely `matched=true`. It rejects modified
bytes, foreign lineage/oracle, path escape/symlink, changed tolerance and forged
PASS. The caller must authenticate/pin the independent reference and lifecycle;
this function returns NUMERICAL_COMPONENT_ONLY, not an end-to-end verdict.

The existing YOLO tensor decoder moved to the shared NumPy-only
`adapters/yolo/tensor_bundle.py`; the example delegates to it. Tensor count,
name length and rank are bounded. Package adapter exports are now lazy while
preserving existing public names, so importing the pure reference/codec no
longer eagerly imports missing `_ndnsf`. Native adapter APIs still require their
real runtime; this change does not substitute stubs or claim native compatibility.
Operator requirements pin the verified host NumPy 1.24.4. Final collection must
use the candidate-pinned implementation/environment, not an arbitrary ambient
checkout of these helpers.

## Verification

Expanded focused suite: **455 passed in 35.86s**:

```sh
python3 -m pytest -q Experiments/TigerCluster/tests \
  tests/python/test_spec183_v3_backend_selection.py \
  tests/python/test_spec183_public_recipients.py \
  tests/python/test_spec180_yolo_numerical.py \
  tests/python/test_spec183_numerical_reanalysis.py --tb=short \
  --junitxml=Experiments/TigerCluster/results/t006-numerical-reanalysis-r1/junit.xml
```

Evidence includes the actual User evidence function (AST-isolated from native
imports), actual shared tensor codec and NumPy comparison, real file retention,
fresh-process import without native bindings, and candidate rejection at the
launcher boundary. Reference arrays in these tests are fixtures, not newly
executed full-model inference. The first run exposed eager package imports;
another assertion incorrectly counted the fixture's package directory as an
unexpected response output and was narrowed to the two actual output files.

T006 still needs full lifecycle, actual per-role/node/GPU/edge observations,
cleanup and negative-case collection. Final worker/operator and T007 remain
incomplete. New source and operator lock must be sealed into the next candidate;
no old SIF is promoted and no remote job was submitted.
