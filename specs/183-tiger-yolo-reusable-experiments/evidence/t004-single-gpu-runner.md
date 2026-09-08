# Single-node GPU allocation runner source wiring

Date: 2026-09-07. Status: IMPLEMENTED / component verification only.

`jobs/yolo/submit.py run` now binds a prepared single-node GPU run to the shared
run root, frozen bundle, I/R/E and profile, and the shared journal's submitted job.
After consuming localSif it enters one finite srun step with one task, one GPU,
profile CPUs and kill-on-bad-exit. The internal `rank` action requires the same
journal job in RUNNING state and enters `execute_single_gpu_run`.

The GPU owner queries real scontrol job/task records before issuer/native work,
then reuses the complete single-node execution owner formerly wired only to CPU:
signed preparation, per-role homes, real rank lifecycle, independent per-request
references, retained outputs, cleanup, and the collection handoff. The existing
NodeRuntime revalidates allocation and probes CUDA before provider startup.
No ACKs, model outputs, or qualification records are manufactured.

The batch retains srun cleanup. Forced kill, unreaped process or nonzero exit
prevents collection; GPU reanalysis checks the retained cleanup and joins its
job ID to the collection's allocation identity. The batch leaves its journal
RUNNING: only an external observer of actual Slurm termination may close it.

Verification:

```bash
python3 -m pytest Experiments/TigerCluster/tests/test_yolo_allocated_runner.py \
  Experiments/TigerCluster/tests/test_yolo_local_execution.py \
  Experiments/TigerCluster/tests/test_yolo_submit.py -q --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-single-gpu-runner-20260907/focused.xml
# Final rank-entry and cleanup/job binding checks:
python3 -m pytest Experiments/TigerCluster/tests/test_yolo_allocated_runner.py \
  -q --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-single-gpu-runner-20260907/boundaries.xml
```

Results: **63 passed in 17.77s**, then **11 passed in 0.69s** for the changed
boundary module (65 unique tests with latest passing evidence). No failed run.
Journal files and state transitions are real in boundary tests. srun, collector,
allocation capture, provision and rank boundaries are explicitly doubled in their
respective composition tests; these passes are not live Slurm/NDN/CUDA evidence.

Still required: remote stage/relocation of exact content and prior retained
evidence, verified shared lock semantics, allocation capacity and scratch staging,
Slurm terminal-state reconciliation, two-node rank orchestration and the registered
negative-dependency case. Submit still returns REMOTE_STAGING_NOT_WIRED; distributed
run remains DISTRIBUTED_RUNNER_NOT_WIRED. T004/T007/T012/T013 remain incomplete.
Other client's host build remained live as PID 1291688, NDNSF 109/153 at observation;
no build was restarted or modified by this change.

Actual profile was rendered after final edits. The public dispatch check exited
78 with integrity VERIFIED / qualification NOT_EVALUATED; retained result:
`Experiments/TigerCluster/results/spec183-single-gpu-runner-20260907/profile-check.json`.
E is `sha256:650d0d84d2a526928b8cf25648d0c5645fc5996be8d049726c85555433f29e49`;
profile digest `sha256:064005b13142a87c7cd989610d15eb91f3303971a2942ecb1a42da8e9f80041a`.
I/R unchanged. CodeGraph sync and active Context Mode health passed. GSD uses the
repository handoff; its previously observed W019 warning remains a tool-layout
limitation, not qualification authority. ARS is not needed for this implementation
boundary change; no statistical experiment design changed.
