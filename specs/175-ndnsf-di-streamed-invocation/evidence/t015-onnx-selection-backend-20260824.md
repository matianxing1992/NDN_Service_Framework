# T015 ONNX/native Selection backend identity correction

**Date**: 2026-08-24  
**Status**: implementation verified; workload process proof remains open

## Defect

The MiniNDN Selection Dataflow V2/V3 preparation path accepted only the
`qwen-transformers` runtime and always wrote `transformers` or
`transformers-cpu` into each Provider residency offer. Consequently an ONNX
or native-ONNX run could pass a syntactically valid offer with a false runtime
identity. That could cause placement to select a capability that did not
match the executable Provider backend.

## Correction

`Experiments/NDNSF_DI_LlmPipeline_Minindn.py` now:

- admits `qwen-transformers`, `qwen-onnx`, and `qwen-onnx-cpu-native` for the
  request-first Selection Dataflow profiles;
- emits `transformers`/`transformers-cpu` only for the Transformers runtime;
- emits `onnxruntime-cuda`/`onnxruntime-cpu` for the ONNX runtimes; and
- fails closed for every other runtime.

The source contract regression in
`tests/python/test_spec175_contract_gate.py::test_minindn_selection_dataflow_preserves_onnx_runtime_identity`
locks the accepted runtime set and the backend mapping.

## Verification

```text
python3 -m py_compile Experiments/NDNSF_DI_LlmPipeline_Minindn.py
python3 -m pytest -q \
  tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_evidence.py \
  tests/python/test_spec175_sif_preflight.py
-> 18 passed, 1 skipped
```

This is a source/contract correction, not a model execution result. A fresh
MiniNDN process with the frozen P1/P2 workload is still required for T015.

## Admission correction

The same review found a second runtime-identity mismatch in
`tools/ndnsf-di/spec168_real_model_gate.py`: the launcher values
`qwen-onnx`/`qwen-onnx-cpu-native` were compared directly with each stage's
`runtime` field. The canonical exported stage manifest records that field as
`onnxruntime`. The gate now maps both launcher values to `onnxruntime`, while
leaving the Transformers value as `qwen-transformers`.

`tests/python/test_spec168_tiny_qwen_fixture.py` now validates both ONNX
launcher spellings against an `onnxruntime` stage manifest. The focused
regression suite reports `22 passed, 2 skipped` including this mutation.
