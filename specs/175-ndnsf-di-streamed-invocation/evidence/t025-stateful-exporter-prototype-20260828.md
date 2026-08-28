# T025 stateful exporter prototype — 2026-08-28

## Result

The stateful Qwen3.5 export path passed a small, mixed-attention CPU
prototype inside the adapter exporter SIF.  One exported ONNX graph accepted
both a variable-length prefill and a one-token decode, and its logits plus all
three state families matched the eager wrapper.  This is implementation
evidence for the graph contract only; it is not G5 qualification for the
Qwen3.6-27B artifact or CUDA/Tiger execution.

## Reproduction

| Field | Value |
|---|---|
| Exporter image | `/project/tma1/ndnsf-di/staging/spec175/qwen36-onnx-exporter/qwen36-onnx-exporter.sif` |
| Exporter Python | Python 3.10, Torch 2.6.0+cu124, ONNX Runtime in the image |
| Model | synthetic Qwen3.5 text-only, 4 layers: 3 linear-attention + 1 full-attention |
| Shape | hidden 64, vocab 128, 4 attention heads, FP32 |
| State I/O | `attention_kv_{in,out}`, `recurrent_state_{in,out}`, `convolution_state_{in,out}` |
| Sequence policy | `stateful-prefill-decode-v1` |
| Prefill/decode | 3 tokens followed by 1 token, same graph and persistent state outputs |

The source under test was copied to `/tmp/spec175_llm_pipeline_lib.py` inside
the image and loaded as `spec175_llm_pipeline_lib`; this was an isolated
prototype invocation and did not modify the staged image.

## Observed parity

The exported graph passed `onnx.checker.check_model` and an ORT CPU session.
Maximum absolute eager-versus-ORT differences were:

| Invocation | Logits | Attention KV | Recurrent state | Convolution state |
|---|---:|---:|---:|---:|
| Prefill (3 tokens) | `1.34e-7` | `7.15e-7` | `1.40e-9` | `1.19e-7` |
| Decode (1 token) | `1.79e-7` | `9.54e-7` | `5.24e-10` | `7.45e-8` |

The graph input contract contained `input_ids`, `attention_mask`,
`position_ids`, and the three state inputs.  The prefill state length grew
from zero to three; the decode output grew it to four.  No host-side model
fallback or per-token state serialization was used by this check.

## Qualification boundary

This prototype closes neither G5 nor T025.  The staged Qwen3.6-27B bundle
remains the previously recorded fixed-context artifact and is rejected by the
stateful manifest contract.  A new adapter-certified 27B export must still be
generated, passed through the manifest builder, parity-checked, embedded in a
new exact SIF, and executed on CUDA before G5 can be marked complete.
