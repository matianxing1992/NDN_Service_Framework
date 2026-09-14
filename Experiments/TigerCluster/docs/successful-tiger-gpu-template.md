# Successful Tiger GPU template

This is the repository copy of the last complete NDNSF-DI YOLO Tiger GPU
candidate. It is a **comparison template**, not current Spec186 evidence. The
full receipt is the historical Spec183 checkpoint referenced below.

## Frozen reference tuple

| Plane | Value |
| --- | --- |
| Base SIF | `base-runtime-controller-version-j4-v23.sif`; SHA-256 `44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0` |
| Base source seal | `sha256:3c8ffcd5146a34d7577fb3712ee9178bf46637adb0e4a6b453c6adbc76979642` |
| APP | v39, read-only; source seal `sha256:112c444243fe80eaa41e17410e6234590cce5f6c91714c5695e8f6ebcdda770d`; manifest `sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7` |
| Runtime | Apptainer 1.5.3; app built with bounded `-O0 -g0 -B/usr/bin` fallback after GCC9 ICE |
| Model/oracle | YOLO26n ONNX; terminal shape `[1,50,6]`; `maxAbsError=0.00042724609375`; model roles CUDA; Merge CPU |
| Single-node | Slurm `210365`, `itiger02`, run `tiger-single-node-gpu-v49-r8`, `NORMAL_EXPERIMENT_PASS` |
| Two-node | Slurm `210366`, `itiger02` + `itiger03`, run `tiger-two-node-gpu-v49-r13`, one warmup + three measured, four roles, nine edges/request, cleanup closed |

Historical source: `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-runtime-checkpoint-20260910.md`
at commit `4fd5936c` (the file may be archived on branches that do not carry
Spec183 documents).

## Reuse procedure

1. Copy this tuple into a new active-Spec candidate draft.
2. Compare every changed plane: source seal, base SIF, app bundle, model,
   harness, profile, Apptainer, node/GPU request, and oracle.
3. Reuse the exact base/app only when their ABI and source identities match;
   app-only changes remain read-only overlays and do not rebuild the base.
4. For every other change, create a new candidate identity and rerun source,
   dependency, exact-SIF, local MiniNDN, and allocated-node preflight.
5. Promote GPU PASS only after request → ACK → Selection → Response, CUDA
   provider evidence, numerical oracle, per-process exits, and cleanup are
   present. A READY/CUDA probe alone is not acceptance.

Do not copy job IDs or results into a new run. Use them only as the expected
evidence shape and as a regression comparison.
