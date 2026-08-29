# Spec175 T025 Tiger stage readiness: current candidate

**Date:** 2026-08-29
**Candidate:** `spec175-runtime-fe285147`
**Source revision:** `15f823a4bb5a158be86df81074aae48d2a0244d7`
**Gate:** G5 stateful-stage/cache-effectiveness readiness

## Bound inputs

- Runtime SIF: `/project/tma1/ndnsf-di/releases/spec175-runtime-fe285147/runtime.sif`
- Runtime SIF SHA-256: `6d905dbb44a65b02dcd091fb1c9a89b81ce7bb86551acf16db2a620c8602e7d7`
- Model root: `/project/tma1/ndnsf-di/artifacts/spec175/qwen36-stateful/spec175-qwen36-stateful-rtx6000-206700`
- Model: `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`
- Model digest: `cf246180e7334baf35b76ef49e3b9ec7626b44f657d05cb4ab5f370d90d473d2`
- Execution provider: `CUDAExecutionProvider`; three RTX 6000 devices; `--mem=96G`

## Result

Tiger Job `206907` completed on `itiger02` with Slurm exit `0:0` in
`00:01:21`. The terminal record is
`/project/tma1/ndnsf-di/submits/spec175-runtime-fe285147/spec175-stage-206907/spec175-gate-terminal.json`
with SHA-256
`0f6a8e1e365289f0a17591fdb13c10bba5df09d38111111d3e54bfbebfe2bebe`.
The machine-readable readiness record is
`.../spec175-stage-206907/tiger-stage-readiness.json` with SHA-256
`d1918ad677d6a64dff9a9c589ccf41cb0d398f939c9bc6c01885aae7f9efc2a7` and
reports `status=PASS`.

The eight-token control produced
`[96445, 96170, 142289, 96895, 13368, 3035, 97186, 96565]` and a nonempty
transcript. All stages used device-resident state, had zero complete-state
host bytes after prefill, and reported zero CPU model-compute fallback:

| stage | role | cached median (ms) | full-prefix median (ms) | pairs |
|---:|---|---:|---:|---:|
| 0 | `/LLM/Pipeline/Stage/0` | 68.4436 | 170.1506 | 3 |
| 1 | `/LLM/Pipeline/Stage/1` | 67.5662 | 169.3090 | 3 |
| 2 | `/LLM/Pipeline/Stage/2` | 79.5844 | 182.4312 | 3 |

Each ORT profile reported `unknownCpuOps=[]`, `cpuCoreOps=[]`, and
`missingCudaCoreGroups=[]`; CPU execution was limited to declared shape-control
operators. Artifact staging verified 317 ledger entries and 3,149,131 stage
bytes in the content-addressed cache. The run closes the current candidate's
T025 control plus G5 readiness prerequisite for T026.

## Retained failed attempts

- Job `206904` failed before SIF execution with
  `SPEC175_STAGE_RUNNER_MISSING`; the minimal remote submit tree omitted the
  runner invoked by the stage wrapper.
- Job `206905` reached the three-GPU job but was `OUT_OF_MEMORY` with the
  default 3-GB allocation (`MaxRSS=3118036K`); it used the same SIF/model and
  no NDNSF-DI request ran. The successful replacement declares `--mem=96G`.

These are submission-bundle/resource failures, not CUDA, ONNX Runtime, model,
or NDNSF-DI protocol results. Historical job `206782` belongs to the superseded
`spec175-final-candidate-replay42c` SIF and is retained only for provenance.
This evidence does not claim T026 multi-Provider execution, T034 conversation
residency, or T027 performance.
