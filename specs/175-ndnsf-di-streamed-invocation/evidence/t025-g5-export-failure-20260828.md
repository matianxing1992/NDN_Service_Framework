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

Tiger Job `206269` was the first authorized retry, using the same model,
runtime SIF, workload, GPU class, and exporter command with only the corrected
source bundle.  It passed staged PyTorch parity and then failed while loading
`stage-0-qwen.onnx` in ORT with `Unsupported type proto value case`.

Job `206269` passed staged PyTorch parity and then failed while loading
`stage-0-qwen.onnx` in ORT with `Unsupported type proto value case`.  A small
probe using the same exporter SIF, FP16, Qwen3.5 stateful wrapper, and the
actual non-divisible attention dimensions passed ONNX checker, ORT load, and
ORT execution.  The remaining distinguishing condition was the
`--containall` temporary filesystem: the same actual-dimension probe failed
with `ENOSPC` until `TMPDIR` was explicitly bound to a writable project path.

The next controlled retry, Job `206330`, added only a bound `/work/tmp` and
`TMPDIR=/work/tmp` for exporter temporary files.  It passed staged PyTorch
parity but failed with the same ORT load error.  The temporary-directory
hypothesis is therefore rejected; no G5 artifact was produced.

## Controlled graph diagnosis

Before another 27B allocation, three bounded CPU probes isolated the failure
boundary using the same exporter SIF and ORT 1.20.0:

| Job | Subject | Result |
|---|---|---|
| `206380` | 21-layer stateful Qwen3.5 graph, small dimensions | ONNX checker and ORT load passed |
| `206383` | 21-layer graph with the Qwen3.6 layer-type pattern | ONNX checker and ORT load passed |
| `206386` | 21-layer graph with Qwen3.6 dimensions (`5120/24/4/256`, actual linear-state widths) | ONNX graph export passed; checker could not resolve external data because the exporter wrote the files outside the graph directory |
| `206392` | Same `206386` graph after placing external files beside the graph, 64-GB CPU ORT load | `EXTERNAL_COUNT=181`, `EXTERNAL_MISSING=0`, `ORT_LOAD_PASS` |

The probes show that stateful Loop types, the 21-layer count, the actual
non-divisible attention head dimension, and large external initializers are
accepted by the same ORT version when the external files are colocated.  The
remaining 206269/206330 failure was consequently a packaging/serialization
boundary defect: the legacy Torch exporter resolves external-data locations
relative to its process cwd, while ORT resolves them relative to the ONNX
file.  The exporter now changes cwd to the artifact directory for the export,
passes the graph basename, restores cwd in a `finally` block, and fails early
with `QWEN_ONNX_EXTERNAL_DATA_MISSING` for a missing, empty, or escaping
external location.

The local regression suite after this correction is:

```text
17 passed, 1 skipped
```

These probes and the local regression close the diagnosis and implementation
correction only.  T025 G5 remains open until a new source-bound exporter
bundle produces all three Qwen3.6-27B stages and passes the registered CUDA
prefill/decode checks.  Jobs `206269` and `206330` remain preserved as failed
evidence; neither is a G5, G6, or performance result.
