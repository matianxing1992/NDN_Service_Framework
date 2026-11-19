# Contract: Qwen3.6 Model and Placement

## Frozen model

```text
model: Qwen/Qwen3.6-27B
revision: 6a9e13bd6fc8f0983b9b99948120bc37f49c13e9
architecture: Qwen3_5ForConditionalGeneration
text architecture: Qwen3_5TextModel
layers: 64
dtype: bfloat16
text-only: true
enable_thinking: false
decoding: greedy
max_new_tokens: 64
```

The vision encoder, MTP, KV-cache optimization, quantization, and sampling are
outside this candidate.

## Runtime contract

- `qwen2` and `qwen3_5` are distinct adapter branches.
- `qwen3_5` requires hybrid linear-attention/full-attention layer support.
- Runtime dependencies and wheel digests are frozen in the release manifest.
- Missing compatible classes, unsupported masks, CUDA unavailability, or CPU
  fallback stops before provider readiness.

## Placement contract

```text
reference: one RTX 5000 node, three allocated GPUs
candidate Stage 0: distinct node/GPU, layers [0,21), embedding
candidate Stage 1: distinct node/GPU, layers [21,42)
candidate Stage 2: distinct node/GPU, layers [42,64), final norm and LM head
```

Each artifact must pass checksum, strict state-dict load, one CUDA forward,
and peak-memory reserve before multi-node execution. Average weight estimates
do not constitute a PASS.
