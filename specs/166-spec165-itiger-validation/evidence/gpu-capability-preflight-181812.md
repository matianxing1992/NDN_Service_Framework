# TigerCluster GPU Capability Preflight — 2026-08-01

## Scope

This is an environment capability probe, not a Qwen, NDNSF-DI, DistributedRepo,
or three-node acceptance result. It was run only after the current-source local
Spec 165 Gate A-D aggregate passed. It did not load a model, start NFD/NDNSF-DI,
publish an Interest, or execute a distributed request.

## Submission

- Slurm job: `181812`
- Partition/GRES: `bigTiger`, `gpu:rtx_5000:1`
- Node: `itiger07`
- Elapsed: `00:00:22`
- Terminal state: `COMPLETED`, exit `0:0`
- Durable remote evidence: `/project/tma1/ndnsf-di/probes/rtx_5000-20260801T214205Z`
- Result marker: `RESULT=PASS`

## Accepted checks

- Host GPU: NVIDIA RTX 5000 Ada Generation, 32,760 MiB, driver `560.28.03`.
- `CUDA_VISIBLE_DEVICES=0`; one GPU was visible to the allocation.
- Apptainer/Singularity was available on the compute node (`1.5.3-1.el9`).
- Compute-node `/tmp` was allocation-scoped XFS with about 14 TiB free.
- The 64 MiB write/fsync probe passed.
- `apptainer exec --nv` exposed the NVIDIA device nodes and the allocated GPU.
- Container-side NVIDIA smoke passed with the CUDA base image
  `docker://nvidia/cuda:12.4.1-base-ubuntu22.04`.
- The probe converted that public capability image to SIF and recorded
  `c84a7e631c1003b76cd80050e35dc8fe9d2063bd3ae96a6962d99ac6b739fec7`.

## Claim boundary

The probe proves only that one RTX 5000 allocation can see NVIDIA hardware,
use Apptainer `--nv`, and write job-local scratch. The converted CUDA SIF is a
probe input, not the NDNSF-DI candidate release and not evidence for the
Qwen/model/workload identities used by the local Gate A-D aggregate. A future
TigerCluster standalone or distributed run must separately bind the accepted
candidate SIF, model digest, workload digest, source revision, request IDs, and
three-node NFD/NDNSF-DI evidence.

## Candidate identity audit

The only currently promoted Spec 166 SIF is
`/project/tma1/ndnsf-di/releases/spec166-dcef2858c060/runtime.sif` with SIF
digest
`sha256:e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a`.
Its materialization record binds the older candidate image ID
`sha256:dcef2858c060ba0ba01903dd57ca5d3e844d1c1f9b500ed59f5dba9dd753ac47`,
not the current local CPU-gate image
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`.
Therefore no candidate-bound standalone job was submitted after this probe;
using the old SIF would violate the current source/image identity boundary.
