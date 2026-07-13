# iTiger runtime release contract

## Build boundary

The release source is an OCI image referenced by digest. Its primary builder is
GitHub Actions Buildx using the existing GPU Dockerfile, exact dependency locks,
checksum-bound sealed Git archives, and `DEPENDENCY_SOURCE_MODE=sealed`. Buildx
pushes the OCI image to GHCR and records its digest, provenance, SBOM, signature,
source seal, and small release evidence. The OCI image itself is not uploaded as
an Actions artifact. An iTiger CPU allocation materializes that exact digest as
SIF under project storage. iTiger does not require or run a Docker daemon.

Rootless Podman/Buildah inside a bounded iTiger CPU allocation is the fallback,
supplied either by an administrator-supported installation or a separately
digest-qualified builder SIF. It consumes the same Dockerfile/lock/seal and uses
explicit graphroot/runroot/cache/tmp paths below selected compute scratch.

Login-node builds and the default `/home/$USER/.local/share/containers/storage`
graphroot are forbidden. Login-node tool discovery is provisional until one
compute-node diagnostic proves scratch write/fsync, rootless build/export,
OCI-to-SIF conversion, execution, teardown, and durable promotion.

Job `147712` is the preserved first diagnostic and failed after allocation start
with `ROOTLESS_BUILD_TOOL_MISSING:podman`. It cannot be retried. Any builder-SIF
replacement must bind the original submission ID, a new identity, explicit human
authorization, the builder OCI/SIF digests, observed Apptainer fakeroot/user-
namespace mode, VFS/chroot settings, and all original output/promotion checks.

## Sealed source and scratch selection

The build input is a read-only source seal containing the top-level source
revision, every dependency revision, Git-archive SHA-256 and byte size, and the
lock digest. Neither CI nor iTiger may clone inside the Dockerfile or silently
substitute a missing revision. Mutation, unsafe archive path, or digest mismatch
fails before build.

Scratch selection is deterministic: use a writable allocation-owned
`SLURM_TMPDIR`; otherwise use the configured Slurm `TmpFS` plus user/job identity;
otherwise use a validated job-unique `/tmp` path. The selected canonical path
must contain the current user and `SLURM_JOB_ID`, must not resolve below `/home`
or `/project`, and is recorded before any container command runs.

## Build evidence

The durable build record contains the Slurm job/allocation identity, node,
source-seal digest, builder and Apptainer versions, selected scratch path,
explicit graphroot/runroot/cache/tmp paths, Dockerfile/lock digests, command
exit states, OCI digest/archive checksum, SIF checksum, secret scan, promotion
state, and teardown state. Failed partial artifacts never replace an accepted
release.

## Required SIF contents

- NFD and ndn-cxx programs/libraries;
- ndn-svs/NAC-ABE dependencies;
- NDNSF C++ runtime and Python binding;
- NDNSF-DI Python/C++ runtime and launchers;
- PyTorch and Transformers/tokenizer tooling;
- ONNX Runtime GPU and compatible CUDA user-space libraries;
- Qwen oracle/export/stage-provider commands;
- evidence and compatibility probes.

## Host/runtime split

| Host provides | SIF provides |
|---|---|
| Slurm allocation | all project applications/libraries |
| Apptainer executable | exact Python and C++ runtime |
| NVIDIA kernel driver/devices | CUDA user-space, PyTorch, ORT GPU |
| project filesystem and selected compute scratch | entrypoints/config templates |

Invocation is `apptainer exec --nv` with a clean environment and explicit bind
allowlist. NVIDIA Container Toolkit is neither required nor installed on iTiger.

## Forbidden image content

Private keys, VPN/SSH credentials, MFA artifacts, registry/model access tokens,
user home directories, mutable model weights, accepted evidence, and secrets.
All Qwen weights and stage exports remain under iTiger `/project`; build-context
rules and workflow tests reject common model-weight formats.

## Acceptance

A release is `PASS` only when source snapshot, build allocation, selected scratch,
non-home rootless storage, OCI digest/archive checksum, SIF checksum, dependency
locks, secret scan, compute-node imports/linking, NFD lifecycle, allocated GPU
correlation, PyTorch CUDA operation, and ONNX Runtime CUDA provider execution all
pass.

## Rollback

The current and prior accepted OCI/SIF digests remain protected. Rollback means
freezing a new run/candidate binding to the prior accepted release; no mutable
`current.sif` replacement may alter an already frozen candidate. Failed SIF
materialization removes only its partial path and never the prior release.
