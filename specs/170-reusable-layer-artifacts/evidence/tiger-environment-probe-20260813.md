# TigerCluster environment probe (2026-08-13)

**Status**: environment discovery only; no Spec170 gate was executed.

The probe jobs were deliberately CPU-only and did not run NDNSF, Apptainer, a
model, or inference. Both jobs used the staged candidate directory
`/project/tma1/ndnsf-di/candidates/spec170-source-59f3b93`.

| Job | Node | State | Result |
|---:|---|---|---|
| 188133 | itiger05 | COMPLETED, 0:0 | environment pass |
| 188134 | itiger05 | COMPLETED, 0:0 | environment pass |

Observed on the compute node:

```text
Apptainer: 1.5.3-1.el9 (/usr/bin/apptainer)
project: 900T total, 61T used, 840T available
/tmp: 14T total, about 101G used
podman: absent
buildah: absent
docker: absent
fakeroot: 1.34
GPU allocation: none
```

The login-node presence of Podman/Buildah/Docker is not a compute-node build
capability. The active iTiger operations contract forbids using the login node
for builds or inference, so the rootless host-tool fallback is stopped at this
probe rather than retried under a different identity.

## Consequence for Spec170

The remote project still has no Spec170 SIF. The available releases are older
Spec160--Spec168 images, which cannot substitute for the current candidate.
The next valid path is an immutable foundation/OCI build through the sealed
release route (or an administrator-provided, separately qualified builder),
followed by Slurm CPU SIF materialization, exact-SIF parity, T029 freeze, and
only then D0 → D1 → D2a/D2b → D2h.

