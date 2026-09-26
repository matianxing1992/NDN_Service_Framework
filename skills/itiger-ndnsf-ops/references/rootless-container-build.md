# Rootless Container Build on iTiger

## Authority and placement

Run every build in a bounded CPU Slurm allocation. The login node is for SSH,
rendering, submission, monitoring, and small metadata operations only. A
successful build does not prove GPU runtime or NDNSF-DI execution.

Use `/project/$USER/ndnsf-di` for source snapshots and durable output. Use only
the current allocation's verified local scratch for Podman/Buildah graph roots,
run roots, cache, unpacking, and compilation. Prefer `$SLURM_TMPDIR`; otherwise
probe `/scratch`, then `/tmp`. Never assume retention after job termination.

## Required pre-build probe

Record all of the following from the allocated compute node:

```bash
hostname
id
podman version
podman info --format json
buildah version
apptainer version
findmnt -T "$BUILD_SCRATCH"
df -h "$BUILD_SCRATCH" "/project/$USER"
```

Create a job-specific directory with mode `0700`, write and fsync 64 MiB, then
build a tiny public base image with rootless Podman. Export it as an OCI archive,
convert or pull it into SIF with Apptainer, execute a static command, and record
SHA-256 checksums. A warning, skipped conversion, login-node result, or image
visibility line is not a PASS.

The first Spec 110 compute probe (`147712`, `itiger01`, 2026-07-13) started and
failed with `ROOTLESS_BUILD_TOOL_MISSING:podman`. Login Podman 5.2.2 and Buildah
1.33.7 are therefore not evidence that these commands exist on compute nodes.
The same probe selected `/tmp`, despite Slurm advertising `TmpFS=/scratch`.
Preserve this negative result; never resubmit the same identity.

No `/etc/subuid` or `/etc/subgid` mapping was observed for the probed account on
2026-07-13. Do not assume arbitrary image ownership works. Preserve the first
probe failure and repair the build strategy under a new diagnostic identity.

Replacement order:

1. Prefer an administrator-supported compute-node Podman/Buildah module or
   installation.
2. Otherwise design and offline-test a pinned builder OCI materialized as a SIF,
   with Buildah `chroot` isolation and VFS storage, then probe it once under a
   newly authorized diagnostic identity. Nested Buildah may still fail without
   the required user namespace/capability support.
3. Direct Apptainer definition builds change the OCI-source contract and require
   a formal Spec revision; do not silently substitute them.

Spec 110's offline-qualified candidate uses the amd64 manifest reference:

```text
quay.io/buildah/stable@sha256:8570703f0feb3f39d180e932a2ec8e350ee860790062a5ecd5a3b3ac51f337c5
```

Render it explicitly with `release build-render --builder-mode apptainer-sif`.
The job must materialize that digest into job scratch, retain the resulting SIF
SHA-256, and run Buildah only through `apptainer exec --fakeroot` with
`--storage-driver vfs` and `--isolation chroot`. Before the tiny build, require
`buildah info` to pass inside the same container and record the effective mode.
`ROOTLESS_BUILD_USER_NAMESPACE_UNAVAILABLE` is an executed negative, not a
reason to alter flags or submit again under the same identity.

## Full build layout

```text
/project/$USER/ndnsf-di/source/<source-id>/
/project/$USER/ndnsf-di/releases/<release-id>/
/project/$USER/ndnsf-di/evidence/builds/<build-id>/

<job-scratch>/spec110-build-<job-id>/
  containers/
  runroot/
  cache/
  work/
  output/
```

Point Podman/Buildah storage explicitly at job scratch. Do not let it fall back
to `/home/$USER/.local/share/containers/storage`. Build from immutable source
and dependency locks. Treat any changed source, base digest, dependency commit,
backend, CUDA version, or build recipe as a new release identity.

## Promotion and cleanup

Before job exit:

1. Secret-scan the source context and release output.
2. Generate the SBOM/provenance and immutable release manifest.
3. Hash OCI/SIF and every retained record.
4. Copy into a new partial directory under `/project`.
5. Verify the durable hashes and atomically rename the partial directory.
6. Record the Slurm job ID, node, source/release IDs, exact command, exit state,
   bytes, duration, and scratch cleanup result.
7. Delete only a path whose canonical prefix equals the current job-specific
   scratch root. Never clean an accepted or referenced project release.

Keep the current and previous accepted SIF protected. A failed build cannot
replace either and is preserved as a negative operational result.
