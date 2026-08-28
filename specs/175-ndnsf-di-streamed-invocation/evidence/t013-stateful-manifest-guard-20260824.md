# T013 stateful manifest guard — 2026-08-24

The remote Qwen3.6-27B candidate at
`/project/tma1/ndnsf-di/artifacts/spec175/qwen36-onnx-202864` was inspected
without modifying it.  Its three stages declare `onnxruntime`, but each stage
has empty `cacheInputs`/`cacheOutputs` and only the legacy `past_key.*` /
`present_key.*` tensors.  No `attention_kv`, `recurrent_state`, or
`convolution_state` input/output family is declared.  The candidate therefore
does not satisfy the Spec175 stateful subject and is not eligible for T025.

The stage-manifest builder now fails closed unless every stage supplies unique
`stateInputNames` and `stateOutputNames` containing all three required families:
`attention_kv`, `recurrent_state`, and `convolution_state`.  It also requires
the sealed `single-token-autoregressive`, `text-only`, `mtpEnabled=false`, and
`thinkingMode=disabled` subject fields and rejects forbidden graph components.
The regression `test_stage_manifest_builder_rejects_legacy_past_key_graph`
proves that the old candidate shape cannot be promoted by metadata alone.

Focused verification:

```text
python3 -m pytest -q \
  tests/python/test_spec175_qwen_stateful_onnx.py \
  tests/python/test_spec168_tiny_qwen_fixture.py \
  tests/python/test_spec175_contract_gate.py
21 passed, 1 skipped
```

This closes the false-admission gap only.  T013/T025 remain open until a new
graph-derived stateful Qwen3.6 export executes prefill plus incremental decode
on CUDA for all three stages.
