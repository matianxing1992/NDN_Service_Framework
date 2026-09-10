# Tiger v69 deployment diagnosis

Status at 2026-09-09: **v69 reached a real allocation but failed the storage
budget gate; v70 is prepared for a bounded retry with the corrected profile**.

## Boundary evidence

| Boundary | Observation | Interpretation |
| --- | --- | --- |
| Exact local SIF + external APP | `tiger-local-cpu-v66` is `PASS` / `NORMAL_EXPERIMENT_PASS`; two requests, candidate `sha256:50f2cca0…30f69`; numerical receipts report `maxAbsError=0.0005340576171875` under `atol=0.001`. | The v22 base plus v32 APP composition is executable locally. This does not prove GPU qualification. |
| Local MiniNDN | v62 local gate is `PASS` / `NORMAL_EXPERIMENT_PASS` with two requests and the same independent oracle bound. | The local multi-process protocol path and collector pass; this is CPU/MiniNDN evidence, not Tiger evidence. |
| Tiger allocated substrate probe | A real `srun` probe reached `itiger04` with Apptainer `1.5.3-1.el9`; the login node reports `1.3.4-1.el9`. The declared profile uses the allocated-node version. | The version difference is a deployment concern, but it is handled by the compute-node owner and has not caused v69's observed failures. |
| Tiger transport | v69 first submit failed before SSH with `TRANSPORT_FILE_ROW`; two shared files had mode `0664`, outside the allowed set. After chmod to `0444`, the 331-file transport plan passed with candidate `sha256:b79c691e…4d30c4`. | Sender-side staging metadata was invalid; SIF bytes and APP bytes were unchanged. |
| Tiger receiver bootstrap | The first real receiver attempt reached the frozen bootstrap and failed with `JOURNAL_ROOT`: declared `/project/tma1/ndnsf-di/locks` did not exist. | Remote project namespace configuration was incomplete; no Slurm or Apptainer workload ran. |
| Tiger retry | The lock root was created with mode `0700`; the retry entered rsync and is transferring the immutable base SIF into `.incoming`. | The deployment has now passed the earlier config boundaries. GPU/Slurm outcome remains pending. |
| Slurm submission | After correcting the received wrapper to executable read-only mode `0555`, the same candidate was accepted as Slurm job `210254` on the `bigTiger` RTX 6000 partition. | The transport/configuration boundary is now crossed; only the job's GPU execution verdict remains. |
| Tiger v69 allocation | Job `210254` ran on `itiger02`, but rank 0 recorded `STAGING_FAILED` / `ValueError: STORAGE_SIF_BUDGET`; declared `3525861376` bytes was below the `3901079552`-byte SIF. | The first runtime failure is a stale profile capacity value, before Provider or CUDA launch; it is not evidence of an invalid SIF or APP. |
| v70 correction | Profile v39 declares `storage.peakBytes=3901079552`, reuses the same v22 SIF by project-storage hardlink, and seals the wrapper at mode `0555`. | The corrected candidate is ready for one fresh Slurm allocation; GPU qualification remains open until its retained verdict. |

## Diagnosis

The observed failures are Tiger project-storage, transport metadata, and then
one stale profile capacity value. They are not evidence of a broken SIF, APP,
or MiniNDN graph: v69 reached an allocation but failed before Provider/CUDA
startup. MiniNDN/Tiger differences remain an unverified runtime risk (allocated
CUDA/ORT, NFD socket, mounts, and scheduler), so v70 is the first corrected
attempt that can produce the requested end-to-end evidence.

The focused transport/SSH/submit regression set passes 92 tests after the
diagnosis, including rejection of invalid modes and journal roots. This checks
the guard behavior; it does not substitute for the still-running Tiger job.

## Required next observation

Run v70 once the receiver accepts its corrected profile, then retain the
receiver preflight, Slurm allocation, Apptainer/CUDA readiness, per-role backend
receipts, numeric oracle, and cleanup records. Only a `SINGLE_NODE_GPU_PASS`
receipt can close T013. A transport `PASS`, v69 allocation, or local MiniNDN
`PASS` must not be promoted to that qualification.
