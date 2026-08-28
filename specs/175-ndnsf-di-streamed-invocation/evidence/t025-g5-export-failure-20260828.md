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

## Current-source packaging verification

The corrected exporter source was copied to a new immutable
`source-stateful-r3` directory.  The local and remote source hashes are
identical (`llm_pipeline_lib.py` SHA-256
`622cd0e1a458d9343f555798d85372c040dfbd90b992e9dff5644a127dec5cda`).

Tiger Job `206400` used that source with the same exporter SIF and exported a
21-layer graph at the actual Qwen3.6 dimensions.  The export produced the
graph plus its external weight files beside it; its diagnostic checker then
failed only because an in-memory `ModelProto` was checked without its graph
directory.  This was a validation-script error, not an exporter error.

Job `206408` reused the unchanged `206400` output and checked the graph by
filename from inside the container.  It passed path-based ONNX checker and
loaded in ORT with six inputs and four outputs.  The job used 64 GB RAM,
completed in 2m49s, and reported `R3_ORT_LOAD_PASS 6 4`; the only stderr
messages were non-fatal ORT thread-affinity warnings.  This closes the
current-source external-data packaging check, but it is not a Qwen3.6-27B
three-stage CUDA export and does not close G5.

## Actual-model graph diagnosis

Tiger Job `206445` used the current `source-stateful-r4` exporter bundle and
the same exporter SIF, model revision, three-GPU allocation, and staged
PyTorch parity checks.  Model download, reference loading, all three stage
package writes, and staged token parity passed.  Stage 0 then failed at ORT
load with:

```text
Unsuported type proto value case.
```

The diagnostic census printed before the exception was:

```text
externalCount=222 externalMissing=0 graphBytes=995166 nodeCount=6186
typeCounts={"sequence_type":32,"tensor_type":90}
stage=0
```

Thus the actual Qwen graph had no missing or escaping external weight and the
failure was not caused by SIF, CUDA, temporary storage, or the Qwen head
dimension.  The 32 sequence TypeProto values came from the scripted recurrent
rule's tensor-list accumulator (`SequenceConstruct`/`SequenceAt`), which the
deployment ORT 1.20 baseline rejects while loading.  Job `206445` ended
`FAILED` after `00:14:15` with `MaxRSS=161587772K`; no artifact was promoted
and G5 remains open.

The correction replaces that scripted list with tensor concatenation and adds
an exporter guard that rejects any future non-tensor graph value with the
explicit `QWEN_ONNX_NON_TENSOR_TYPE` error.  Local focused tests remain
`17 passed, 1 skipped`.  A bounded CPU SIF probe using the corrected source
(Tiger Job `206466`, 32 GB, 22 s) exported the 21-layer stateful graph with
`bad=[]`, passed ONNX checker, and loaded in ORT; its graph census contained
only tensor value types.  This is a correction/probe result, not a 27B G5
qualification; a new source-bound 27B export is required before any CUDA
artifact or downstream G6 work.

These probes and the local regression close the diagnosis and implementation
correction only.  T025 G5 remains open until a new source-bound exporter
bundle produces all three Qwen3.6-27B stages and passes the registered CUDA
prefill/decode checks.  Jobs `206269` and `206330` remain preserved as failed
evidence; neither is a G5, G6, or performance result.
