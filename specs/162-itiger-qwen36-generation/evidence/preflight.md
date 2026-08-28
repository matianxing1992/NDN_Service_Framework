# Spec 162 Preflight Evidence

## Status

`PAUSED_ARCHITECTURE_DEPENDENCY_AFTER_T005_FAILURE`

This file retains the historical T002–T005 preflight record. The coherent
OCI/SIF runtime and no-model local/RTX operation-status gates completed, but
Job 175053 later failed before model download because the installed SDK path
was absent from `PYTHONPATH`. No Qwen model weights were downloaded or loaded,
no generation was attempted, and no three-node allocation was submitted.
Further live work is paused on `specs/163-di-collaboration-planning`.

## T002 local compatibility evidence

Completed on 2026-07-28 without model weights or GPU work:

- added an additive `qwen3_5` text-stage loader while preserving `qwen2`;
- normalized the nested `text_config` from
  `Qwen3_5ForConditionalGeneration`;
- mapped `model.language_model.*` weights into strict stage-local modules;
- implemented Qwen3.6 four-plane position IDs and per-layer
  `full_attention`/`linear_attention` masks;
- passed `past_key_values` using the Qwen3.6 decoder contract;
- made `enable_thinking=False` explicit in local chat-template formatting;
- retained fail-closed CUDA selection and added a Transformers 5.14.1 minimum
  capability check;
- added the inactive candidate overlay lock
  `packaging/ndnsf-di-container/oci/layered/locks/qwen36-overlay.lock.json`.

Focused validation:

```text
tests/python/test_spec162_qwen36_stage.py        11/11 PASS
tests/python/test_spec160_qwen_stage_device.py    3/3 PASS
tests/python/test_spec161_qwen_generation.py     12/12 PASS
python3 -m py_compile                             PASS
```

The test sequence retained the intended RED checkpoint (8 missing-contract
errors before implementation, then PASS). The base runtime still pins
Transformers 4.48.2; T003 applies the separately digest-locked Qwen3.6 overlay
with Transformers 5.14.1 and its complete 19-wheel closure.

## T003 coherent runtime and binding evidence

Completed on 2026-07-28 without model weights:

- overlay lock:
  `sha256:aedbff59b78a23f2288b8228d18a9d45dee7194cf13e5fa6d19b02311c2dc5c4`;
- local image:
  `sha256:59c71dedf8736daf2e363b4b10b8b113c63c5d50ee0fe83619b283975eaf87c7`;
- immutable OCI:
  `ghcr.io/matianxing1992/ndnsf-di@sha256:6216ef2b9525740423d67b5327933b59de284b01930aeccde3a657b3c0288f29`;
- sealed archive:
  `sha256:bfac393e381f43ff954e49a70d48adc4a05f11fdf2584f58498f6f1f4f51254b`;
- promoted SIF:
  `/project/tma1/ndnsf-di/releases/spec162-6216ef2b9525/runtime.sif`;
- SIF SHA-256:
  `6bb55d85bd1244e30d0233d84c753be887e527695422ba47df9ec89c20c1d072`.

The layered build manifest records `developmentCandidate=true` because the
source seal was created from the current dirty workspace. The exact app source
seal is
`sha256:bcc175cd8c99ef250150379f127645327216816844d97eb0e30fc92edb0dccc1`.
This is a frozen development candidate for T003/T004 validation, not a clean
Git or production release.

The first standalone runtime-probe invocation failed because it omitted the
required `--lock` argument. The next probe exposed a real read-only-container
portability bug: `pip check` emitted an unwritable-cache warning and the probe
misclassified it as a dependency conflict. The verifier now disables the pip
cache and version check while continuing to reject unexpected dependency
errors. Overlay tests pass 4/4 and the rebuilt candidate passed in a read-only
container.

The local Docker gate passed with 4 GiB memory, 5 GiB memory+swap, two CPUs,
one fake stage, one real collaboration assignment, and the complete
`report_operation_status()` sequence 1, 2, 3, 4. Evidence:
`results/spec162-itiger-qwen36-generation/local-docker-operation-status-20260728T162T003B/`.

Anonymous compute-node access to the private GHCR image remains unavailable
(`401 Unauthorized`). The exact Docker archive was therefore streamed
directly to `/project` without creating a local tar. Slurm Job 174578 converted
it in allocation scratch and atomically promoted the 4.1 GiB SIF:

```text
job=174578
node=itiger04
state=COMPLETED
exit=0:0
elapsed=00:12:47
gpu=none
```

Slurm Job 174594 then ran the SIF with `apptainer exec --nv`:

```text
job=174594
node=itiger07
state=COMPLETED
exit=0:0
elapsed=00:00:53
gpu=1 x NVIDIA RTX 5000 Ada Generation, 32760 MiB
slurm-memory=5G
runtime=fake
model-load=disabled
cudaAvailable=true
operation-status=1,2,3,4
```

Both SIF jobs retain checksum-bound evidence under
`/project/tma1/ndnsf-di/evidence/spec162/`; small local mirrors are under
`results/spec162-itiger-qwen36-generation/`. Spec 161 Job 174382 remained
`PENDING (Resources)` and was not modified.

## Frozen user configuration

- three distinct RTX 5000 candidate nodes;
- one stage and one GPU per node;
- maximum 64 generated tokens per prompt;
- five real prompts;
- one excluded warmup plus five measured generations per prompt;
- full answer, TTFT, every inter-token latency, total latency, and tokens/s.

## Official model discovery

- Model: `Qwen/Qwen3.6-27B`
- Revision observed: `6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`
- Model type: `qwen3_5`
- Architecture: `Qwen3_5ForConditionalGeneration`
- Text layers: 64
- BF16 parameters reported by Hugging Face: 27,781,427,952
- License tag: Apache-2.0
- Frozen ranges: `[0,21)`, `[21,42)`, `[42,64)`
- Text-only, `enable_thinking=False`, greedy, EOS, 64-token ceiling

Official sources were read on 2026-07-28:

- https://github.com/QwenLM/Qwen3.6
- https://huggingface.co/Qwen/Qwen3.6-27B
- https://huggingface.co/Qwen/Qwen3.6-27B/raw/main/config.json

## Read-only iTiger discovery

Observed on 2026-07-28:

- partition `bigTiger` is UP with maximum walltime 28 days;
- nodes `itiger07` through `itiger11` each advertise eight `rtx_5000` GPUs;
- same-node three-GPU reference and three-distinct-node candidate placements
  are schedulable resource shapes, not reservations or execution evidence;
- Spec 161 Job 174382 remained `PENDING (Resources)` and was not changed.

Read-only refresh at `2026-07-28T06:28:49-05:00`:
`174382|PENDING|(Resources)|2026-07-28T23:58:57`.

## Code-aware pre-implementation audit

**Verdict**: `CONDITIONAL PASS` for local implementation; `BLOCK` for live
execution.

| Severity | Finding | Controlling action |
|---|---|---|
| HIGH | Current stage loader imports `Qwen2DecoderLayer` and rejects every model type except `qwen2`. | T002 must add and test an additive `qwen3_5` hybrid-layer adapter. |
| HIGH | Current coherent SIF pins Transformers 4.48.2; the configured local pip mirror ends at 4.46.3; official Qwen3.6 support is present in current Transformers 5.14.1. | T003 must build one digest-locked coherent Python 3.10 runtime and repeat binding smokes. |
| MEDIUM | Dividing BF16 parameter bytes suggests three stages fit 32 GB, but embedding/head, activations, masks, and runtime workspace are not covered by average arithmetic. | T004 must measure strict CUDA load, forward, and peak allocated/reserved bytes for each stage. |
| MEDIUM | Qwen3.6 defaults to thinking and officially recommends much longer outputs. | Freeze concise prompts and `enable_thinking=False`; treat missing EOS at 64 as truncation. |

No Core/security redesign is required. The existing secured collaboration,
operation-status, and dependency-reference path remains the owner.

## Gate boundary

T003 and the T004 implementation/no-download scratch gate are closed. The
original T004 quota decision remains retained as a negative result. The user
subsequently authorized measured global NFS free space as the T005 development
capacity authority without claiming a verified user quota. The first T005
preparation identity, Job 175053, failed before model download due to a missing
SDK `PYTHONPATH`; its first terminal state is retained. A corrected replacement
identity requires new explicit authorization. T007 remains a later independent
authorization boundary.

## T004 preparation implementation and no-download capacity gate

Completed on 2026-07-28 without model download or model compute:

- added the frozen five-prompt direct-response contract with
  `enableThinking=false`;
- added text-only `Qwen3_5ForCausalLM` reference loading across one node's
  three RTX 5000 GPUs using BF16, an explicit device map matching the exported
  `[0,21)`, `[21,42)`, and `[42,64)` stage cuts, 30 GiB per-device maximums,
  SDPA, greedy decoding, `use_cache=false`, EOS, and 64 tokens;
- made preparation fail closed when the full-model reference layer placement
  differs from the immutable stage manifest.  A prior large campaign exposed
  why this is required: its reference map used `[0,18)`, `[18,42)`, `[42,64)`
  while the stage artifacts used `[0,21)`, `[21,42)`, `[42,64)`.
- extended stage export to select both Qwen2 flat keys and Qwen3.6 nested
  `model.language_model.*` text keys while excluding vision and MTP;
- added stage package generation for `[0,21)`, `[21,42)`, and `[42,64)`;
- added strict per-stage CUDA load, one forward, allocated/reserved peak
  measurement, zero CPU fallback, and at least 1 GiB device headroom;
- added a fail-closed preparation allocation requesting one node with exactly
  three RTX 5000 GPUs and no H100 resource;
- passed local and exact-image tests:

```text
tests/python/test_spec162_qwen36_preparation.py  4/4 PASS
tests/python/test_spec162_qwen36_stage.py       12/12 PASS
read-only Transformers 5.14.1 image tests       4/4 + 12/12 PASS
Python compile and shell syntax                 PASS
```

The no-download capacity probe used frozen source digest
`sha256:6a4629a4640df8b82ac748fb532a1db7baad91983628cac530e7c4be1cdb4643`:

```text
job=174610
node=itiger07
state=COMPLETED
exit=0:0
gpus=3 x NVIDIA RTX 5000 Ada Generation, 32760 MiB each
scratchFreeBytes=15251694170112
scratchTemporaryPeakRequiredBytes=171798691840
downloadPerformed=false
modelComputePerformed=false
```

The scratch gate passed. The durable gate remains blocked:

```text
currentUsageBytes=81003067904
projectedDurableBytes=158312479232
projectedDurableWithReserveBytes=179787315712
verifiedQuotaBytes=0
blockReason=DURABLE_QUOTA_UNVERIFIED
```

`/project` is NFS. `quota -s` returned no quota, `lfs` and `mmlsquota` are
unavailable, and `xfs_quota` cannot inspect the NFS mount. The filesystem-wide
free capacity is not substituted for an authoritative user/project quota.
Therefore T005 preparation is not authorized or admissible yet. The local
script contract is ready, but actual Qwen3.6 checkpoint loading, reference
generation, stage package sizes, CUDA forward behavior, and peak memory remain
unverified until T005.
