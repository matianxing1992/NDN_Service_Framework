# TigerCluster exact-SIF CUDA preflight (2026-08-14)

- Slurm job: `189262`, node `itiger01`, exit `0`, peak RSS `4307480K`.
- Allocation: one GPU (`--gres=gpu:1`), Apptainer `1.5.3-1.el9`, exact SIF,
  `--nv`, no model weights.
- SIF SHA-256:
  `sha256:525c4b890c4012d3f36653d0209f7decec508635818e5b0829250ef06d012af1`.

The image probe reported:

```text
status: PASS
cpuFallback: false
GPU: NVIDIA H100 80GB HBM3
UUID: GPU-90597ffb-6498-9a24-ca98-18fbdc33c447
driver: 560.35.03
Torch CUDA: 12.4
ONNX providers: CUDAExecutionProvider, CPUExecutionProvider
profile providers: CUDAExecutionProvider
compatibility: PASS
```

ONNX Runtime emitted non-fatal `pthread_setaffinity_np` warnings because the
Slurm CPU mask did not contain all requested affinity indices. The probe still
executed the CUDA kernel, observed the CUDA provider in the ONNX profile, and
matched the allocated GPU UUID. This is a bounded T027 CUDA preflight; it is
not T031/D1 and is not a complete distributed-inference workload.
