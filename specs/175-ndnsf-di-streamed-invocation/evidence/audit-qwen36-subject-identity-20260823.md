# Spec175 Qwen3.6 subject-identity audit — 2026-08-23

Status: `CONTRACT_AND_UNIT_PASS`; real Qwen3.6 ONNX export and CUDA execution
remain open under T013/T025/T026.

## External identity verification

The official model repository exists at
<https://huggingface.co/Qwen/Qwen3.6-27B>, and the pinned revision resolves at
<https://huggingface.co/Qwen/Qwen3.6-27B/tree/6a9e13bd6fc8f0983b9b99948120bc37f49c13e9>.
The official model card describes a 64-layer hybrid causal language model with
a vision encoder and optional MTP support. Therefore a bare model/revision pin
does not uniquely identify the Spec175 execution subject.

## Audit correction

Spec175 now additionally freezes:

```text
decodeMode=single-token-autoregressive
modality=text-only
mtpEnabled=false
thinkingMode=disabled
precision=float16
```

The pinned placement profile no longer advertises unsupported `bfloat16`; its
graph identity includes precision, decode mode, modality, and MTP state. The
offline stateful-ONNX sealer requires the same fields and rejects declared
vision/image/video or MTP/speculative components. It also requires disabled
thinking mode and the exact chat-template digest.

## Verification

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q \
  tests/python/test_spec175_qwen_stateful_onnx.py \
  tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec168_large_model_gate.py
```

Result after the workload-identity extension: `20 passed`.

The positive sealer test records text-only, single-token, MTP-disabled,
thinking-disabled fields and a canonical chat-template digest. Independent
mutations to modality, MTP, decode mode, thinking mode, chat-template digest,
vision-projector presence, and MTP-head presence fail. The fixed adapter profile
is asserted as FP16, and the older large-model adapter regressions still pass.

The refreshed expected-negative G0 manifest has SHA-256
`57a11657c35cf8c63926ddd474487c30effd5be1902ce5e694529abcb1f1ebdc`.
Its only blocker is the current dirty in-scope tree; no Qwen subject-identity or
contract-marker blocker remains. Source file hashes at this checkpoint:

- placement profile:
  `5d7a2d1e56ece125a2f188efddf07d196449dc76cb996222299cc8a0720e2505`;
- offline sealer:
  `b9a5aa4152fd9322f86703a8bdf0f941253fb534e589212ce794e1b8b7295b1a`.

## Boundary

The sealer validates a declared graph-component inventory; it does not yet
derive that inventory by parsing the real 27B ONNX graph. T013 must add that
graph-derived proof before G5, and G5 must verify the loaded CUDA ORT graph
closure rather than trust metadata alone.
