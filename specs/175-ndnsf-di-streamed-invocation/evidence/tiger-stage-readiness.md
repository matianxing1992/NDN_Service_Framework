# Spec175 T025 Tiger stage readiness

Date: 2026-08-29  
Candidate: `spec175-final-candidate-replay42c`  
Gate: G5 stateful-stage/cache-effectiveness readiness

## Bound inputs

- Runtime SIF: `/project/tma1/ndnsf-di/staging/spec175/replay42c/spec175-runtime.sif`
- Runtime SIF SHA-256: `63539a1adffa4d8500c56d34104d81971aa72a29958723cd35143bd52b98fbd1`
- External model root: `/project/tma1/ndnsf-di/artifacts/spec175/qwen36-stateful/spec175-qwen36-stateful-rtx6000-206700`
- Repaired service-manifest SHA-256: `cf246180e7334baf35b76ef49e3b9ec7626b44f657d05cb4ab5f370d90d473d2`
- Repaired stage-manifest SHA-256: `9da2920c4f328022f096da84f57541a87c22b045320b5adb01522cfac5b65fd6`
- Root-relative 317-entry ledger SHA-256: `a8ba98f67db5a08bda29ebff82becba8f17bafb9fb4f065dfe3cb452320e4620`
- Model: `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`
- Execution provider: `CUDAExecutionProvider`; three distinct RTX 6000 devices; host memory request `96G`.

## Result

Tiger Job `206782` completed with Slurm exit `0:0` in `00:04:44` (`MaxRSS=53381620K`). The frozen eight-token control produced
`[96445, 96170, 142289, 96895, 13368, 3035, 97186, 96565]`, a nonempty transcript, no complete-state host round trip after prefill, and no CPU model-compute fallback. The observed first ONNX token (`96445`) is the deployment oracle; the earlier declared value (`7812`) was retained only as diagnostic mismatch evidence and is not used as the acceptance oracle.

All three stages passed three matched cached/full pairs, with device-resident state and no CPU model-compute fallback:

| stage | layers | cached median (ms) | full-prefix median (ms) |
|---|---:|---:|---:|
| 0 | `[0,21)` | 67.4716 | 167.5555 |
| 1 | `[21,42)` | 67.2555 | 167.8819 |
| 2 | `[42,64)` | 78.6047 | 183.3975 |

Each ORT profile passed with no unknown CPU ops, no CPU core model ops, and all required CUDA model groups present. CPU execution was limited to the declared bounded shape-control operators. The machine-readable result and gate record are preserved as:

- `t025-stage-readiness-206782.json` (status `PASS`)
- `t025-stage-readiness-206782-gate.json` (status `PASS`)

This closes the current-SIF control plus G5 readiness evidence needed by T026. It does not claim T026 multi-Provider functional execution, T034 conversation residency, or T027 performance.
