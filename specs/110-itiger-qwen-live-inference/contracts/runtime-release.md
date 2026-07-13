# iTiger runtime release contract

## Build boundary

The release has two immutable inputs. `Dockerfile.foundation` is built and tested
locally from the exact lock and checksum-bound sealed Git archives, then pushed
to GHCR by a source-revision-bound tag and consumed only by digest. The
dispatch-only GitHub Buildx job uses `Dockerfile.gpu` to add the SHA-256-pinned
ONNX Runtime GPU SDK, locked CUDA/Python closure, and native ONNX adapter. It
must not clone or rebuild NFD, ndn-svs, NDNSD, OpenABE, RELIC, or NAC-ABE.
Buildx records the foundation and final digests, provenance, SBOM, signature,
and small release evidence. Package visibility is configured public by the
operator before dispatch; an empty-credential manifest lookup is a release gate
because iTiger must not receive a registry secret. An iTiger CPU allocation
materializes the final digest as SIF; iTiger does not require or run a Docker
daemon.

The ABI base is Ubuntu 20.04 with OpenSSL 1.1.1, matching the measured working
local OpenABE/NAC-ABE installation. Foundation, CUDA devel, and CUDA runtime
images are pinned to Ubuntu 20.04 digests; mixing an Ubuntu 22.04/OpenSSL 3 base
is rejected before build.

Ubuntu 20.04's system Python 3.8 cannot install the locked PyTorch 2.6 and
Transformers 4.48 stack. The GPU stage therefore copies Python 3.10.18 from one
digest-pinned official Bullseye slim image with the same glibc 2.31 ABI. PPA,
host Python, and an unpinned installer are forbidden. The copied interpreter is
tested on the pinned Focal base for SSL, readline, SQLite, and unresolved ELF
dependencies. Its explicit runtime closure is `libgdbm6`, `libreadline8`,
`libsqlite3-0`, and `libssl1.1`; unused NIS and Tk extensions are removed rather
than importing Bullseye-only or irrelevant system libraries.

Rootless Podman/Buildah inside a bounded iTiger CPU allocation is the fallback,
supplied either by an administrator-supported installation or a separately
digest-qualified builder SIF. It consumes the same foundation digest/GPU lock and uses
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

The local foundation build input is a read-only source seal containing the top-level source
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
