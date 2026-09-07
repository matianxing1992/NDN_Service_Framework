# Slurm task binding implementation checkpoint

2026-09-07, branch TigerClusterExperiments. Component evidence only;
T006/T007 remain open. No allocation or model job was submitted.

## Site observations, not a positive allocation test

Read-only SSH confirmed `/usr/bin/scontrol`, Slurm 24.05.2 and the
data_parser/v0.0.41 JSON interface on cluster `itiger`. The account's squeue
was empty. Queries for nonexistent job 999999999 and step 999999999.0 both
returned exit0 with empty errors/warnings, but jobs=[] and steps=null.
Therefore exit0, or merely absence of an errors entry, cannot qualify a job.
These commands neither allocated resources nor ran inference on the login node.

Field shapes were checked against the exact upstream Slurm 24.05.2 parser:
https://github.com/SchedMD/slurm/blob/slurm-24-05-2-1/src/plugins/data_parser/v0.0.41/parsers.c
Job state is a flag array; node_count uses the UINT32_NO_VAL wrapper; the step
id is serialized through SLURM_STEP_ID_STRING/SELECTED_STEP, while number_tasks
is UINT32. Positive fixtures are source-shaped, not captured running jobs.

## Implemented boundary

`runtime/yolo_allocation.py::capture_task_allocation` executes read-only
scontrol show job/step plus host-list expansion, with a single remaining-time
budget, an explicit system binary and a restricted command environment.
It never calls sbatch/srun or builds a container. Missing/mismatched job env
is rejected before the first command. JSON results must contain exactly one
record, no errors/warnings, the observed parser and the expected cluster.

Independent expected inputs are the submitted jobId/submissionKey from the
durable SubmissionJournal and frozen profile cluster.partition/gpuClass.
The worker derives rank and node count from its normal case (1 or 2 nodes).
Checks include current uid, exact submission comment, RUNNING job and step,
GPU type/count, distinct hosts, matching expanded job/step host lists, actual
hostname at the given rank, task count/ranks and exactly one step GPU.
Compressed host-list spelling is not compared as physical identity.

SLURM_STEP_GPUS is retained as a global ID, separately from the task's
CUDA_VISIBLE_DEVICES selector. They need not be equal; neither is treated as
a physical UUID. This distinction follows the Slurm srun environment contract:
https://slurm.schedmd.com/srun.html (SLURM_STEP_GPUS and SLURM_JOB_GPUS).

`NodeRuntime.verify_allocation` writes exclusive slurm-allocation.json bound
to runId, preparation and runtime candidate. It retains exact scheduler
payloads, host expansions and only the 11 required task environment values
(never a full environment dump). The GPU probe records this allocation file's
digest. A different launch selector is rejected, not silently rewritten.

`run_normal_node` now checks allocation before the CUDA probe, network setup
and Provider startup. GPU calls require allocation_expected from the outer
operator; None fails closed. Local CPU does not query Slurm. Tests assert an
allocation failure results in cleanup with zero GPU/Provider launches.

## Remaining controlling work

- Wire the final operator to the actual SubmissionJournal job and frozen
  partition/gpuClass; do not derive expected jobId/comment from task env.
- Read and semantically revalidate the retained allocation and GPU receipts
  from trusted staging hashes, join their digests/nonce/selector/UUID, and feed
  the result to live/retained role collectors. Hash consistency alone is not
  scheduler provenance. Do not fabricate these receipts to unlock submission.
- Verify all positive field shapes on an actual permitted srun task after
  T007 and the local/SIF gates. Current source-shaped fixtures cannot establish
  runtime compatibility, hostname alias behavior or physical GPU assignment.
- Finish certified graph coverage and final operator/negative-case wiring,
  then full local and Tiger qualification in the required order.

This does not claim positive allocated-GPU verification, current NDNSF-DI
inference, or completion of the experiment entrypoint.

## Tests

Expanded focused regression: 793 passed in 50.62s, JUnit
Experiments/TigerCluster/results/t006-slurm-allocation-r1/junit.xml.
Same TigerCluster/tests plus six Python backend/public-recipient/numerical/
candidate/execution-plan selectors as preceding checkpoints. Afterwards added
a single-node rank0/task-count1 fixture: allocation subset 33 passed in 0.33s.
All positive scheduler data in these tests is explicitly synthetic; command
capture, worker gate and ordering tests use declared boundary doubles.
