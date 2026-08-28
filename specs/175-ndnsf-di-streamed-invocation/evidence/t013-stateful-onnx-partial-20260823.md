# T013 stateful ONNX contract (partial)

**Date**: 2026-08-23  
**Status**: partial; CUDA/Qwen3.6 qualification remains open

## Implemented

- `DecodeStateIdentityV1` binds model, graph, artifact, adapter, tokenizer,
  runner, role split, layer range, prefix/position, precision/layout, state
  schema/components, runtime ABI, security domain, Provider boot, request,
  attempt, cache, and generation identity.
- `DecodeStateBundleV1` requires both full-attention KV and
  recurrent/convolution state and matches component digests to the identity.
- `DecodeStateTransaction` admits only a contiguous candidate with immutable
  request/attempt bindings, advancing prefix/position digests, an unchanged
  cache epoch, unchanged component layout, and downstream admission; failed
  candidates do not replace committed state.
- Python `PersistentStatefulOnnxSession` owns one ORT session and validates
  prefill/incremental feeds. The C++ ONNX runner now rejects a declared
  stateful manifest whose exact graph signature or any of the three state
  families is incomplete.
- `export_spec175_qwen36_stateful_onnx.py` seals a content-addressed ONNX
  manifest without importing PyTorch or Transformers.

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q \
    tests/python/test_spec175_qwen_stateful_onnx.py \
    tests/python/test_spec175_qwen_generation.py
13 passed

./waf build --target=integration-tests -j2
success
```

The remaining acceptance boundary is real Qwen3.6-27B stateful export,
provider-local tensor-state hit/miss/recompute evidence, and CUDA ORT
prefill plus incremental continuity. The tiny CPU fixture and this manifest
sealer cannot close T025.
