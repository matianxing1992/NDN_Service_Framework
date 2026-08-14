# TigerCluster Qwen3-compatible runtime probes (2026-08-14)

## Candidate binding

- OCI: `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:f1c8288e26d3dd7700b9519e51296fc3ed17027c60780fac4b361f470c628a26`.
- Source revision: `a46abe9110ff816145a42b42e9b365d847e41135`.
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135/runtime.sif`.
- SIF SHA-256: `sha256:5e556c17492957d07cde1debad5f7d93d794d43f87b3186914065d53e812fb4c`.
- Model: Qwen3-0.6B revision `e6de91484c29aa9480d55605af694f39b081c455`, model digest `sha256:a317ec50b9a20ebf83a96379016e227dbe83c0b7116e97cfffdfc0bcee4c86db`.

## No-GPU static probe

Slurm job `189302` completed on `itiger01` with no GRES and no `--nv`. The
command used `--cleanenv --containall --home /tmp --pwd /tmp` and
`/usr/local/bin/ndnsf-di-probe-runtime --mode static`. It exited zero and
reported `status: PASS`, including:

- `transformers: 4.51.0`;
- `transformers.models.qwen3: present`;
- `transformers.models.qwen3.configuration_qwen3: present`;
- `torch 2.6.0+cu124`, `onnxruntime 1.20.0`, all NDNSF-DI imports and native
  binaries present;
- `modelWeightsIncluded: false`, as required for an image-only probe.

## GPU probe

The first corrected-mode attempt, job `189303`, retained a launch failure:
the image accepts `static` and `allocated-gpu`, not `cuda`; it exited 2 with
the argparse invalid-choice error. Job `189304` used `allocated-gpu` but
`--cleanenv` removed `SLURM_JOB_ID`; the probe correctly returned
`RUNTIME_GPU_PROBE_REQUIRES_SLURM` and exited 4. Neither failure changes the
candidate image.

Job `189305` reran the exact SIF on `itiger09` with one Slurm GPU and passed
after explicitly forwarding only `SLURM_JOB_ID` into the clean container:

- GPU: NVIDIA RTX 5000 Ada Generation;
- UUID: `GPU-84ba2af4-da27-b890-5bbd-f3bd1d33e718`;
- driver `560.28.03`, Torch CUDA `12.4`;
- ONNX Runtime providers: `CUDAExecutionProvider`, `CPUExecutionProvider`;
- profiled provider: `CUDAExecutionProvider`;
- `cpuFallback: false`, compatibility `PASS`.

The ONNX Runtime thread-affinity warnings were non-fatal; the CUDA provider
was observed and the job exited zero.

## Standalone same-model Qwen3 reference

Job `189306` ran `/opt/ndnsf/bin/run-qwen-reference.py` against the external,
content-addressed model directory with `--nv`, greedy decoding, seed `109`,
and `--max-new-tokens 2`. It completed zero on `itiger09` and produced one
complete row:

- prompt: `Explain NDNSF-DI pipeline inference.`;
- input token IDs: `[840, 20772, 38444, 2448, 37, 9420, 40, 15301, 44378, 13]`;
- output token IDs: `[3555, 374]`;
- output digest: `sha256:d6e7e388458351e97bb8987d7798ab29d034d0a0fdfe24212cfafe0e07d57b18`;
- generation time: `0.6929570436477661` seconds;
- result JSON SHA-256: `sha256:e06cbfa2efd3a9697afba510d526b7c71a8fe7f77326cc6cbc44898b7d0ea4a4`;
- prompt JSON SHA-256: `sha256:128b6d22e7f298e33572902e09c01a2c4521fddfc70511e86e5e236881697100`.

Transformers emitted non-fatal warnings about unused sampling parameters and
CuBLAS deterministic configuration. They do not invalidate this bounded
standalone reference, but the formal repeated workload must freeze the
reproducibility environment before claiming deterministic tokens.

These probes establish candidate static, CUDA/ONNX, and same-model standalone
readiness. They do **not** establish MiniNDN permissions, multi-Provider
collaboration, or the complete Spec170 Gate B response.
