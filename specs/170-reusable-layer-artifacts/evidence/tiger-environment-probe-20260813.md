# TigerCluster environment probe (2026-08-13)

**Status**: historical environment discovery only; no Spec170 gate was
executed. This 2026-08-13 snapshot is not the active toolchain contract and
must not be used to select an Apptainer version or a build route. The active
2026-08-17 route is the locally built SIF recorded in
`evidence/local-sif-build-route-20260817.md`.

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

## Historical consequence (superseded)

At that time the remote project still had no Spec170 SIF, so the next path was
recorded as a remote materialization route. That plan is superseded: Spec170
now starts with a locally built application SIF, followed by local closure
checks, one hash-verified upload, and TigerCluster execution only. The observed
`1.5.3-1.el9` Apptainer value remains historical diagnostic data and is not an
active release input.
