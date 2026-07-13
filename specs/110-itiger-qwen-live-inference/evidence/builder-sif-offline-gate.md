# Builder-SIF fallback offline gate

**Date**: 2026-07-13  
**Tasks**: T148-T149  
**Verdict**: `PASS` for implementation and offline tests only; compute-node
fakeroot/user-namespace capability remains `UNVERIFIED` until T151 executes.

The renderer now accepts an explicit `apptainer-sif` builder mode and binds the
amd64 Buildah builder manifest:

```text
quay.io/buildah/stable@sha256:8570703f0feb3f39d180e932a2ec8e350ee860790062a5ecd5a3b3ac51f337c5
```

The Slurm job materializes this OCI reference into a job-scratch SIF, records
its SHA-256, probes `buildah info` through `apptainer exec --fakeroot`, and
forces VFS storage plus chroot isolation. The existing Dockerfile and lock graph
remain authoritative. Host Podman remains available as a separate explicit or
auto-selected backend when the allocated node actually provides it.

## Verification

```text
python3 tools/ndnsf-di/run_spec110_offline_tests.py \
  --output /tmp/spec110-offline-builder-sif
  SPEC110_OFFLINE tests=75 failures=0 errors=0 skipped=0

bash tests/container/itiger-qwen-live/integration/test_rootless_build.sh
  ROOTLESS_BUILD_PIPELINE_PASS

shellcheck \
  packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh \
  tests/container/itiger-qwen-live/integration/test_rootless_build.sh
  exit=0
```

The integration matrix covers the original host builder, a missing-host-tool
builder-SIF path, digest binding, SIF execution, VFS/chroot command capture,
user-namespace rejection, runtime-SIF partial cleanup, scratch teardown, and
evidence-write failure with rollback of the promoted diagnostic artifacts.

## Authority boundary

- `builderSifOfflineGate=PASS`
- `computeUserNamespace=UNVERIFIED`
- `replacementRendered=NOT_EXECUTED`
- `replacementSubmitted=NOT_EXECUTED`
- `nextTask=T150_AFTER_EXPLICIT_AUTHORIZATION_DIGEST`

This gate does not overwrite or retry job `147712`, does not claim an OCI/SIF
was built on iTiger, and does not authorize `sbatch`.
