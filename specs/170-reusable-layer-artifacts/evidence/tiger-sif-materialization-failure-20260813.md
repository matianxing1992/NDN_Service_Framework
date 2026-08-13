# Spec170 TigerCluster SIF materialization failure — 2026-08-13

This is retained negative evidence for SIF materialization. It is not a
runtime or GPU candidate failure.

- Cluster: `itiger05` / `bigTiger`
- Slurm job: `189118`
- OCI reference:
  `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916`
- Resources: 1 node, 4 CPUs, 8 GiB memory, no GPU GRES
- Result: `OUT_OF_MEMORY`, exit `0:125`, elapsed `00:02:16`
- Completed stages: OCI fetch, extraction, Apptainer configuration, SIF
  creation start
- Failure: `mksquashfs` exited 137 and Slurm reported an OOM kill
- SIF/record status: no accepted SIF or record was produced; the materializer
  cleanup path removed the partial output

The next bounded attempt may use the same immutable OCI digest with a larger
CPU memory allocation. It must retain this job as the first failure and verify
the final SIF/record independently before any Gate-D submission.

## Follow-up failure

- Slurm job: `189120`
- Node: `itiger02`
- Resources: 1 node, 8 CPUs, 32 GiB memory, no GPU GRES
- Result: `FAILED`, exit `1:0`, elapsed `00:03:53`, peak RSS `26055028K`
- Failure stage: Apptainer `mksquashfs` while creating the SIF
- Slurm did not report an OOM kill; the log contained no additional fatal
  diagnostic before `mksquashfs` exited
- SIF/record status: no accepted SIF or record was produced; partial cleanup
  passed

The next attempt must build the SIF on node-local scratch and promote it only
after the local SHA-256 is complete. Direct project-NFS `mksquashfs` output is
not an accepted materialization path.

## Local-scratch build / promotion failure

- Slurm job: `189123`
- Node: `itiger02`
- Resources: 1 node, 8 CPUs, 64 GiB memory, no GPU GRES
- Result: `FAILED`, exit `1:0`, elapsed `00:03:57`, peak RSS `26198844K`
- OCI fetch, extraction, and local node-scratch `mksquashfs` completed
  successfully: `Build complete: /tmp/ndnsf-di-189123/spec170-runtime.sif`
- Local SIF SHA-256 reported by the job:
  `sha256:6fad73896e651fccf67ffc32898b85e40ff7a287a716737c562a7e5f4405d7f0`
- Failure occurred after local build and before the promotion confirmation;
  no project SIF/record remained after cleanup

The exact promote command did not emit a diagnostic before the shell exited.
The next bounded attempt must print source/destination sizes and hashes around
the project copy, force a filesystem sync, and preserve the local artifact or
its failure metadata if promotion fails.

## Promote-diagnostic failure

- Slurm job: `189131`
- Node: `itiger02`
- Resources: 1 node, 8 CPUs, 64 GiB memory, no GPU GRES
- Result: `FAILED`, exit `1:0`, elapsed `00:03:58`, peak RSS `26282232K`
- Local `mksquashfs` again completed successfully; local SHA-256 was
  `sha256:3d814beddb32e73b7e1bc1878fe17c66e57e6baf830afd4dc919227e204e0506`
- The job exited before the first promote diagnostic line; the project SIF
  and record were absent afterward

The independent 64-MiB local-scratch-to-project probe `189133` passed with
matching size and SHA-256, so generic project write permission is not the
cause. The next attempt must distinguish a large-file `cp` failure from
`sync`/post-copy validation and preserve the phase result.

## Durable project-capacity finding

After job `189131`, uploading a 2-KiB diagnostic script to the same release
directory failed immediately with `Disk quota exceeded`. This was independent
of SIF size. The corrected 64-MiB copy probe (`189133`) produced matching
source and destination hashes, but does not prove that the multi-gigabyte SIF
promotion is admissible.

The `/project` NFS mount reported `900T` total and `840T` free, but the cluster
does not provide an authoritative per-user quota command in this session:
`quota -s` produced no record, while `lfs` and `beegfs-ctl` were unavailable.
Shared `df` is therefore not a quota authority. Durable Spec170 SIF promotion
is blocked until the storage administrator provides the user/project quota or
frees/expands it. Existing models, SIFs, and evidence were not deleted.
