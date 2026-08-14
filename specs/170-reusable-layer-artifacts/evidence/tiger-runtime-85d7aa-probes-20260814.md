# Tiger runtime probes for candidate 85d7aa4 — 2026-08-14

## Candidate binding

- source revision: `85d7aa475217cabbcfa92ad28612db99cd36e77b`
- OCI: `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:7ab382b75a743b6599b4250e5fb3171289e887177304b6de3759d3278fd75d0c`
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-85d7aa475217cabbcfa92ad28612db99cd36e77b/runtime.sif`
- SIF SHA-256: `sha256:2318c5c27675fa875d5a437f2942000e4dd8e8f5d8fbb4e889c99aef4c5796b9`
- materialization job: `189330` on `itiger01`, exit `0`

## Static and GPU probes

Slurm `189331` (no GPU, `itiger02`) passed the exact-SIF static probe. It
reported the native binaries and all required Python imports, including the
modules that were missing from the previous candidate:

- `ndnsf_distributed_inference.retry`
- `ndnsf_distributed_inference.runtime_v1`
- `ndnsf_distributed_inference.runtime_v1_evidence`

It also reported `torch 2.6.0+cu124`, ONNX Runtime `1.20.0`, Transformers
`4.51.0`, Qwen3 configuration imports, and `status: PASS`.

Slurm `189332` (one GPU, `itiger09`) passed the allocated-GPU probe. The
allocated device was an NVIDIA RTX 5000 Ada, UUID
`GPU-84ba2af4-da27-b890-5bbd-f3bd1d33e718`; ONNX Runtime profiled
`CUDAExecutionProvider`, `cpuFallback=false`, and compatibility was `PASS`.

## Qwen3 standalone probe

- `189333`: retained wrapper failure. The model completed inference, but the
  wrapper attempted to write its result through a read-only bind.
- `189334`: retained wrapper failure. The corrected read-only bind allowed the
  model to produce the result, but the host-side status check incorrectly
  tested `/output/...` after the container exited.
- `189335`: corrected run passed on `itiger01` in `11 s`.

The successful result contains output token IDs `[3555, 374]`, output token
digest `sha256:d6e7e388458351e97bb8987d7798ab29d034d0a0fdfe24212cfafe0e07d57b18`,
and result JSON SHA-256
`sha256:0caff9ddf49b12b841cb87204be99774e1d2461d0e8afe60520fadeef772ce62`.

These probes close candidate static/CUDA/Qwen readiness. They do not prove
NDN permissions or a distributed request/response.
