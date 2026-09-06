# T026 exact-SIF ORT session probes — 206919 and 206920

**Date:** 2026-08-29  
**Candidate:** `spec175-runtime-fe285147`  
**Purpose:** isolate the Qwen CUDA session-construction boundary after the
multi-Provider Provider processes in job 206918 segfaulted before readiness.

## Results

Both probes used the promoted SIF directly with Apptainer 1.5.3 on `itiger02`
and the same read-only Qwen3.6-27B stateful artifact root used by T026. The
probe imported `ndnsf`, imported the Provider facade, and created the ORT
session with the Provider's current options: graph optimization disabled,
single intra/inter-op threads, memory pattern disabled, memory reuse enabled,
`CUDAExecutionProvider` followed by `CPUExecutionProvider`, and one explicit
CUDA device ID.

| Job | Scope | Result | Observed |
|---|---|---|---|
| `206919` | one process, Stage 0, one RTX 6000 | PASS, exit `0:0` | ORT `1.20.0`, CUDA EP first, session created in `5525.164 ms` |
| `206920` | three concurrent processes, Stages 0/1/2, one RTX 6000 each | PASS, exit `0:0` | all three sessions created from the shared project model root in `5511.857`–`6196.469 ms` |

Each session reported `CUDAExecutionProvider` first and the expected stateful
inputs/outputs. The concurrent probe therefore rules out a basic exact-SIF
CUDA/ORT load failure, a missing external-data file, and a three-process
project-filesystem load failure. The ORT warning about inserted CUDA memcpy
nodes is an execution-profile/performance concern, not a load failure.

## Boundary and interpretation

These probes did not start NFD, construct `APPProvider`, perform certificate
bootstrap, or execute a request. They cannot close T026. Together with job
`206918` (three Provider processes segfaulting after `constructor_done` and
before `LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY`), they narrow the current
failure to the **NDNSF Provider initialization plus Qwen ONNX preload
boundary**, rather than the SIF, CUDA driver, ORT package, model files, or
project-storage concurrency in isolation.

The probe scripts are retained under the local diagnostic staging area and
the remote directory
`/project/tma1/ndnsf-di/probes/spec175-ort-session-20260829/`. They are not
functional T026 invocations and no prefill/decode or throughput result is
claimed from them.
