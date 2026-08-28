# Job 182513: outer rank wrapper still required model bindings

- Candidate: `20260804T161603Z-v85-model-free-canary`
- Source identity: `sha256:907da916bfc95ba76c3135eff2d89a4582820f3514ab21177c7cc963eefe8234`
- Source bundle: `sha256:2c6e872e09bf751d65a6abbaa667804a4d8e96a692d631efe8852246b2e5ca38`
- Campaign: `spec168-campaign-v3-98f96fc131edf8ab9f8e`
- Slurm job: `182513`, one admitted submission, no retry
- Slurm result: `FAILED`, elapsed `00:01:52`, rank step `00:00:01`

## Narrowest failure boundary

The model-free batch preflight passed and created `canary-mode.txt`; no stage
manifest, artifact checksum, generation campaign, or model digest evidence was
created.  `srun` then started all three ranks, but
`spec168-three-node-rank.sh` still required `SPEC168_ARTIFACT_DIR` before
Apptainer launch.  All ranks exited at outer-wrapper line 9, before NFD,
controller, Provider, User, Request, ACK, Selection, Response, Repo, or CUDA.

This is the next locally detectable launcher-layer contract defect.  It proves
that correcting only batch and inner compatibility layers was insufficient.

## Repair and prevention

The outer wrapper now applies the same canary/model branch as the batch and
inner layers.  In canary mode it copies only the deployment policy into rank
scratch and passes no artifact mount, generation campaign, model identity, or
workload identity to Apptainer.  A local executable test runs the real outer
wrapper with fake `nvidia-smi` and `apptainer`, all model variables unset, and
asserts that the expanded container invocation contains no model mount or
environment argument.  Normal model mode retains its model bind/env array.

Job 182513 remains immutable.  Requalification requires a new candidate and
campaign; this failed campaign is never resubmitted.

