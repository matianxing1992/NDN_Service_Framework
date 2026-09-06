# T040 Opaque Provider State Handle — 2026-09-01

## Status

T040 implementation acceptance is **PASS**. This record is focused
implementation evidence; it does not authorize G0--G3, SIF, or Tiger
qualification while T031, T035--T036, and T042 remain open.

## Production path repaired

`NativeOpaqueStateHandleV1` is now the shared adapter/worker/runtime contract.
The handle carries only Provider identity, boot identity, request session,
role, state epoch, and an opaque versioned token. It contains no tensor bytes,
device pointer, prompt, or generated text.

`ProviderRoleWorker` obtains and validates the handle after the real runner call.
`NativeProviderRuntime` stages the handle as a bounded UInt8 control bundle,
checks its Provider/boot/session/role/epoch against the exact
`DecodeStateIdentityV1`, and rejects a supported opaque runner that does not
return a handle. CPU runners retain the existing host `TensorBundle` path.

The ONNX CUDA adapter retains `Ort::Value` state in its request-scoped
`deviceStateBySession` map and keeps a parallel `stateHandleBySession` map.
Successor epochs bind the retained device values directly; release erases both
maps. The complete CUDA allocation is not serialized into the coordinator or
NDN dependency path. Handle creation requires the runtime Provider identity and
boot ID, so an unbound adapter cannot silently qualify.

## Focused validation

All commands were run from the repository root after rebuilding the current
`Experimental` tree:

```text
./waf build -j1 --targets=unit-tests
  build finished successfully (104/104)

./build/unit-tests --run_test=NativeProviderRuntimeCarriesOpaqueStateHandleWithoutHostRoundTrip \
  --log_level=test_suite --report_level=short
  1 test case passed; 15 assertions passed

./build/unit-tests --log_level=test_suite --report_level=short
  604 test cases passed; 61,378 assertions passed

python3 -m pytest -q tests/python/test_spec175_contract_gate.py
  15 passed

./waf build -j1 --targets=integration-tests
  build finished successfully (46/46)

./build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI01OneProvider'
  1 case passed; 88 assertions passed
./build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI02TwoProviderEpochCoordinator'
  1 case passed; 32 assertions passed
./build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI03FourProviderEpochCoordinator'
  1 case passed; 49 assertions passed
```

The focused C++ case drives the real `ProviderRoleWorker` and
`NativeEpochCoordinator` paths for two epochs, verifies that epoch two
receives a compact UInt8 handle, verifies atomic commit and release, and
asserts that the complete host state output is never used as the state
transport. Its negative branch advertises opaque support but omits the handle;
the runtime raises `PROVIDER_OPAQUE_STATE_HANDLE_MISSING` and leaves zero
committed entries. Identity, role, and epoch mismatch checks are also enforced
before candidate staging.

The I01/I02/I03 processes remain CPU ONNX oracle coverage and therefore do not
claim physical CUDA transfer measurements. T025/T026 own that later
measurement; T040 now owns the production contract and fail-closed behavior
that makes such a measurement meaningful.
