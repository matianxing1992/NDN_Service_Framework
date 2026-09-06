# Spec180 T007 Role-Kind Validation Evidence

**Status**: `PARTIAL_IMPLEMENTATION` (native end-to-end assembly remains open)

The generic Python `RoleAssemblySpec` and certified ONNX recipe now distinguish
layer/rank roles from graph-component roles. `COMPONENT_SET` requires
`layer_begin=layer_end=0` and a non-empty sorted canonical `node_indices` set;
pipeline/tensor roles still require a non-empty layer interval. The native V3
JSON projection applies the same rule before accepting a role.

Focused command:

`PYTHONPATH=NDNSF-DistributedInference python3 -m pytest -q tests/python/test_spec180_role_assembly.py tests/python/test_spec170_plan_sealer.py tests/python/test_spec175_v3_boundary.py tests/python/test_spec170_layer_reuse_first.py`

Result: `38 passed`.

The changed native source also compiled in the existing configured toolchain:

`./waf build --targets=ndnsf-di-core-objects -j2`

Result: `build finished successfully` (27/27 objects, including
`NativeExecutionPlanJson.cpp` and `NativeCanonicalOnnxAssembler.cpp`). A native
integration execution and Python/native wire-parity assertion remain required
before T007 can close.
