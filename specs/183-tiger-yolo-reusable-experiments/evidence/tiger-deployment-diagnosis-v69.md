# Tiger v69 deployment diagnosis

Status at 2026-09-09: **transport retry in progress; no Tiger GPU verdict yet**.

## Boundary evidence

| Boundary | Observation | Interpretation |
| --- | --- | --- |
| Exact local SIF + external APP | `tiger-local-cpu-v66` is `PASS` / `NORMAL_EXPERIMENT_PASS`; two requests, candidate `sha256:50f2cca0…30f69`; numerical receipts report `maxAbsError=0.0005340576171875` under `atol=0.001`. | The v22 base plus v32 APP composition is executable locally. This does not prove GPU qualification. |
| Local MiniNDN | v62 local gate is `PASS` / `NORMAL_EXPERIMENT_PASS` with two requests and the same independent oracle bound. | The local multi-process protocol path and collector pass; this is CPU/MiniNDN evidence, not Tiger evidence. |
| Tiger transport | v69 first submit failed before SSH with `TRANSPORT_FILE_ROW`; two shared files had mode `0664`, outside the allowed set. After chmod to `0444`, the 331-file transport plan passed with candidate `sha256:b79c691e…4d30c4`. | Sender-side staging metadata was invalid; SIF bytes and APP bytes were unchanged. |
| Tiger receiver bootstrap | The first real receiver attempt reached the frozen bootstrap and failed with `JOURNAL_ROOT`: declared `/project/tma1/ndnsf-di/locks` did not exist. | Remote project namespace configuration was incomplete; no Slurm or Apptainer workload ran. |
| Tiger retry | The lock root was created with mode `0700`; the retry entered rsync and is transferring the immutable base SIF into `.incoming`. | The deployment has now passed the earlier config boundaries. GPU/Slurm outcome remains pending. |

## Diagnosis

The observed deployment failures are Tiger project-storage and transport
configuration failures. They are not evidence of a broken SIF, APP, or MiniNDN
graph. The first failure occurred before SSH; the second occurred in the
receiver before site inspection and Slurm. MiniNDN/Tiger differences remain an
unverified runtime risk (allocated CUDA/ORT, NFD socket, mounts, and scheduler),
but they have not caused the observed failures because no workload reached
those stages.

## Required next observation

Let the same candidate finish transfer, then retain the receiver preflight,
Slurm allocation, Apptainer/CUDA readiness, per-role backend receipts, numeric
oracle, and cleanup records. Only a `SINGLE_NODE_GPU_PASS` receipt can close
T013. A transport `PASS` or local MiniNDN `PASS` must not be promoted to that
qualification.
