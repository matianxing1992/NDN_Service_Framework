# T025 G5 stateful export failure and correction — 2026-08-28

## Failed subject

Tiger Job `206232` used the sealed exporter SIF and the frozen Qwen3.6-27B
revision, but failed during the pre-export staged PyTorch stateful parity
check.  No deployment artifact was promoted and no G5/G6/G7 result is claimed.

The decisive traceback was:

```text
RuntimeError: Sizes of tensors must match except in dimension 2.
Expected size 213 but got size 256 for tensor number 1 in the list.
```

It occurred in `_StatefulExportCache.update` while concatenating a full-
attention KV cache.  The Qwen3.6-27B configuration declares
`hidden_size=5120`, `num_attention_heads=24`, and `head_dim=256`.  The exporter
had incorrectly derived the cache width as `5120 // 24 = 213`; the attention
projection produces width 256.  This is an exporter/state-contract bug, not a
CUDA, Apptainer, ONNX Runtime, or NDNSF-DI runtime failure.

## Correction

Commit `dcf0e660` changes the Qwen state and ONNX dummy-contract construction
to honor the explicit configuration `head_dim`, with a validated fallback only
for configurations that omit it.  It also adds a regression test using the
non-divisible Qwen3.6-27B dimensions.  The focused suite passes:

```text
16 passed, 1 skipped
```

The updated source was copied into a new immutable staging directory
`source-stateful-r2`; local and remote SHA-256 values match, and the exporter
SIF accepted both files through an AST parse and `--help` check.

## Retry state

Tiger Job `206269` is the single authorized retry, using the same model,
runtime SIF, workload, GPU class, and exporter command with only the corrected
source bundle.  It is pending allocation at the time of this record.  Its
result must be checked before G5 can be marked complete.

