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

The first GPU retry with this correction (Job `206469`) reached the staged
PyTorch parity check but reported `prefill 220 != 332` and `decode 695 !=
7812`.  Review showed that the tensor accumulator had preserved the wrong
axis order (`[batch, heads, value, sequence]`) even though it removed the
Sequence TypeProto.  The follow-up fix appends along the sequence axis and
restores the original `[batch, sequence, heads, value]` output layout.

Tiger CPU Job `206498` then ran the updated 21-layer probe in the same
exporter SIF.  It reported `bad=[]`, passed ONNX checker and ORT load, and
passed numerical prefill plus one-token decode comparisons against the eager
wrapper (`PROBE_NUMERIC_PREFILL_DECODE_PASS`).  This closes the tensor-layout
regression at the probe level; it is not yet a 27B G5 result.

These probes and the local regression close the diagnosis and implementation
correction only.  T025 G5 remains open until a new source-bound exporter
bundle produces all three Qwen3.6-27B stages and passes the registered CUDA
prefill/decode checks.  Jobs `206269` and `206330` remain preserved as failed
evidence; neither is a G5, G6, or performance result.

After the axis-order correction, the current source was rerun through the
aggregate Python regression boundary:

```text
PYTHONPATH=. pytest -q tests/python/test_spec175_*.py \
  tests/python/test_spec168_provider_generation.py
217 passed, 1 skipped
```

This is a local source regression result only; it does not promote the pending
Tiger Job `206503` or close the Qwen3.6-27B CUDA gate.

## 206503 exporter-script failure and bounded retry

Tiger Job `206503` used the same SIF and current `llm_pipeline_lib.py`.  It
passed the three-GPU check, model acquisition, full-model reference load,
three staged Transformer package writes, and staged prefill/decode parity
(`332` and `7812`).  Stage 0 ONNX prefill also passed (`maxAbs=0.125`, token
`3994`).  The job then failed before ONNX decode parity because the staged
loop overwrote the shared `decode_ids` with a CUDA tensor; the later CPU-ORT
check called `.numpy()` and raised:

```text
TypeError: can't convert cuda:2 device type tensor to numpy. Use Tensor.cpu()
to copy the tensor to the host first.
SPEC175_EXPORT_RC:1
```

The failure is an exporter-script state-isolation defect, not a model, SIF,
CUDA, or ONNX graph failure.  The script now keeps canonical decode IDs on
CPU and creates a per-stage device view.  The bounded retry is Job `206555`,
using source bundle `source-stateful-r10`; its exporter SHA-256 is
`59492deaf93be75bcfe75460999879d16b7914dbe2c02572a42b5d243c7ba836`, while
the library SHA-256 remains
`a6cae91ad0c7694e554a3d4cacc77ea4d31376c1e0c9326c2fcee95b4f5ca2d5`.
Job `206555` must finish before any further candidate is submitted.

Job `206555` did finish the full three-stage export and all ONNX prefill and
decode parity checks before the post-export assertion.  The manifest's graph
outputs include the ordinary activation `hidden_states_out` in addition to
the three persistent state outputs; the generic `endswith("_out")` check
therefore rejected a valid manifest at its state-name assertion.  The
production exporter now filters state names against the three canonical state
families and publishes the corresponding input/output vectors in matching
order.  This preserves the positional state transaction contract while
excluding ordinary activation outputs.

The focused regression after this correction is `32 passed, 1 skipped`.  A
single follow-up candidate, Job `206563`, uses source bundle
`source-stateful-r11`; its library SHA-256 is
`7a04a18147ea8679172fc8cd558c1e36b073360bd4e9064087892fab265b6203`, and it
retains the r10 exporter SHA-256.  No additional GPU candidate will be
submitted until `206563` reaches a terminal state.

## 206563 stateful Qwen3.6-27B export result

Job `206563` reached `COMPLETED` with exit `0:0` after `00:31:32` on
`itiger04`.  The job used the exact exporter SIF (sidecar SHA-256
`9bbc6e43fb509d957d0aa597ea8b66f5b74982d1ef741884857018b1f6fc4eba`,
`8307560448` bytes), three `NVIDIA RTX 6000 Ada Generation` GPUs, and the
`source-stateful-r11` bundle.  The source hashes recorded by the job are
preserved in the remote evidence directory.

The full reference and staged Transformer paths produced the expected tokens
`332` (prefill) and `7812` (decode).  All three stateful ONNX stages then
exported and passed the exporter-side CPU-ORT numerical checks:

```text
stage 0: prefill maxAbs=0.125, decode maxAbs=0.015625, bytes=1032239
stage 1: prefill maxAbs=0.125, decode maxAbs=0.03125,  bytes=1030786
stage 2: prefill maxAbs=0.05859375, decode maxAbs=0.03125, bytes=1085732
SPEC175_ONNX_EXPORT_COMPLETE
SPEC175_MANIFEST_OK 7812
```

The promoted artifact is
`/project/tma1/ndnsf-di/artifacts/spec175/qwen36-stateful/`
`spec175-qwen36-stateful-rtx6000-206563` and is about 24 GiB.  Its manifest
identifies `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`,
`stateful-prefill-decode-v1`, `single-token-autoregressive`, `text-only`,
MTP disabled, and `qwen-causal-position-v1`.  Every stage exposes exactly the
canonical state vectors:

```text
attention_kv_in, recurrent_state_in, convolution_state_in
attention_kv_out, recurrent_state_out, convolution_state_out
```

No `.pt` file is present.  During promotion, a packaging defect was found:
the first manifest and checksum file still contained the temporary
`.partial` absolute root.  The original files are retained in the evidence
directory as `manifest-before-path-repair.json` and
`artifact-checksums-before-path-repair.sha256`.  The final manifest was
rewritten to the atomic final root, its stage paths were verified to exist,
and a relative-path checksum file was regenerated.  The final verification
contains 316 entries with zero failures.  The final manifest SHA-256 is
`1b7d540508d088eaa21d6b3ed64c60227030a6f431ff410ce487ab927471fd30` and the
final checksum-file SHA-256 is
`ffb0bbb2478bd3715043db953fcc23c8594d5db513a43d43689b9052de1bae5e`; both
are also copied into the remote evidence directory.  The tracked
copier now performs this path rewrite before returning, and a local regression
covers the partial-to-final promotion case.

This closes the source-bound 27B stateful export and artifact-integrity
correction, but it does **not** close T025 G5: the exporter-side ONNX checks
used CPU ORT.  The exact promoted candidate must still pass the pre-frozen
current-SIF control and the registered CUDA-ORT stage-readiness checks,
including device-resident prefill/decode state and cache-effectiveness
evidence, before T025 can be marked complete or G6 can start.
