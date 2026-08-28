# Spec170 real CPU ONNX collective qualification (2026-08-19)

This is local qualification evidence for the adapter-specific 3A path. It is
not a Tiger result, a GPU result, or a T029 release freeze.

## Fixture and environment

The deterministic fixture generator is
`tests/fixtures/spec170/generate_cpu_onnx_fixture.py`. It creates a float32
`x -> y=x+1` model and a two-output `x -> (y=x+1,z=x+2)` model. The focused
run used:

```text
linear.onnx       sha256=6662d53fad7b8a0f0a63d7bc8d28619194af73c3252a2c3ae1139a5f7db1ee53
linear-multi.onnx sha256=f3fd6a0c21bd46194cea8f7d9fc9b85c95c2f888eb9a7b34e152123f5218464f
slice.onnx        sha256=b4ba342fd355a170e2065370aa8eb2751be70826d1cd8e2b8b95023c9624b347
unsplit.onnx      sha256=8d03d770756703398c7797be4f47c62d60ccd959abd293bf87ee2f3240b1b3cd
ONNX Runtime C++: /opt/onnxruntime, CPUExecutionProvider
```

## Evidence

The direct adapter smoke passed:

```text
build/examples/di-native-onnxruntime-smoke linear.onnx
NDNSF_DI_NATIVE_ONNXRUNTIME_SMOKE_OK 2,3,4
```

Four real C++ adapter cases passed with the fixture: model execution,
encoded-input decoding, encoded multi-output packaging, and backend build
state. The new collective case runs two independently created real
`OnnxRuntimeModelRunner` instances through `ProviderRoleWorker`, waits for
both authenticated ranks, checks both `x+1` outputs and CPU
`ExecutionEvidence`, and requires whole-group completion:

```text
NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-cpu-fixtures/linear.onnx \
  ./build/unit-tests --run_test=DistributedInferenceCollectiveRuntime \
  --report_level=short --log_level=message
8 test cases; 2,336 assertions; PASS
```

With `NDNSF_DI_TEST_ONNX_SLICE_MODEL=slice.onnx` and
`NDNSF_DI_TEST_ONNX_UNSPLIT_MODEL=unsplit.onnx`, the focused group also
executes a real two-rank sliced role: rank 0 receives `[1,2]`, rank 1 receives
`[3,4]`, their outputs are assembled, and the result is compared elementwise
with the independent `[1,2,3,4]` unsplit model:

```text
./build/unit-tests --run_test=DistributedInferenceCollectiveRuntime \
  --report_level=short --log_level=message
9 test cases; 2,348 assertions; PASS (real sliced/unsplit oracle enabled)
```

The complete post-change local suites also passed:

```text
unit-tests:        495 test cases; 59,978 assertions; PASS
integration-tests:  34 test cases;     477 assertions; PASS
```

The same fixture now drives the current-source production D2b path through
two `NativeProviderHandler` instances, rather than only the local Worker:

```text
NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-integration.sB0BBP/linear.onnx \
  ./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse
1 test case; 36 assertions; PASS
```

The complete `Spec170NdnsfDiCoreFlow` run with the fixture enabled passed
`26/26` cases and `387/387` assertions. It verifies one final response,
request-scoped group coordination, three real CPU execution-evidence records,
and no CPU fallback. The paired production capability and DATA_V1 SVS
tamper negatives also pass in the same current build. The CPU identity
normalization is covered separately by
`NativeProviderRuntimeReadinessAcceptsCanonicalCpuDeviceId` (`4/4` assertions).

## Scope boundary

The post-change full unit suite includes the provider-local epoch-key access
negative: a runtime bound to `/provider/P1` rejects an attempt to read the
wrapped key addressed to `/provider/P0`. This closes the previously missing
local adapter-specific CPU numerical
qualification for two ranks and adds a current-source production D2b positive
lifecycle. Production SVS drop, duplicate, and reorder bridge cases now also
pass, but this does not prove the full 3A unsplit oracle across all transport
variants, CUDA/NCCL execution, full 3C mutation coverage, T029 freeze, or T036
performance optimality.
The r23 image is usable for its sealed revision only; any uncommitted source
change still requires a new exact-source SIF before Tiger execution.
