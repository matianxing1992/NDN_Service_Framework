# NDNSF-DI Runtime Package

> **Spec170 starts with a local SIF.** Build the complete application SIF on
> the local host with the matching Apptainer release, verify that exact file,
> and upload only the hash-bound SIF. TigerCluster only verifies and executes
> it. Use the Spec170 quickstart as the operator checklist.

This package is the implementation surface for the NDNSF-DI runtime release.
For the current iTiger path, the first artifact is a complete SIF built on the
local host with `adapters/slurm-apptainer/scripts/build-local-sif.sh`. Runtime
execution uses the immutable SIF through the `slurm-apptainer` adapter. The
legacy archive converter is retained under the explicit
`build-sif-from-archive-legacy.sh` name and is not a current release step.

The package also retains separately scoped adapters:

- `slurm-apptainer` for bounded iTiger allocations (the Spec170 path); and
- `docker-compose` for independent long-lived cloud-host deployments.

The cloud adapter and older OCI recipes are not inputs to a Spec170 release.

The package owns local runtime construction, lifecycle integration, profile
validation, and deployment evidence. It does not own NDNSF-DI planning,
provider selection, NDN security, inference-provider selection, or physical
production acceptance. Those behaviors remain in the runtime and Spec 106.

The existing `packaging/ndnsf-di-systemd/` package remains the host rollback
surface. Private identities, tokens, passwords, environment-specific routes,
models, SIF files, and generated evidence must never enter the OCI build
context or Git history.

## Layout

```text
bin/            operator CLI
lib/            common contracts and adapters
schemas/        checked-in runtime schemas
oci/            OCI/Docker recipes for other or historical workflows (not the
                current Spec170 iTiger release input)
adapters/       runtime templates (added by their story phases)
```

The normal local-SIF entry point is:

```bash
packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --definition /path/to/sealed-runtime.def \
  --sif /path/to/release/runtime.sif \
  --record /path/to/release/local-sif-build-record.json \
  --source-seal /path/to/release/source-seal.json \
  --apptainer /absolute/path/to/compute-matched/apptainer \
  --expected-apptainer COMPUTE_PACKAGE_VERSION
```

`COMPUTE_PACKAGE_VERSION` must come from a bounded Slurm allocation on the
target compute partition. The Tiger login-node version is not authoritative.
The script rejects a local/compute semantic-version mismatch and records the
exact local executable path and SHA-256 in the build record.

The definition may use a qualified local base SIF, but it must build the
complete application runtime. After local static/import/closure checks pass,
copy only `runtime.sif` and its records to project storage; Tiger verifies the
hash and executes the file.

## Container ABI boundary

“Build the SIF locally” means that the host drives Apptainer; it does **not**
make the host Python or host libraries authoritative for files executed inside
the image. Build `_ndnsf.so` and every other compiled Python extension inside
the candidate SIF build stage, or inside a sealed builder rootfs proven
ABI-identical to the final SIF. Record the build-side Python executable/version,
`SOABI`, `EXT_SUFFIX`, include root, compiler, glibc, native-library roots, and
extension hash, then compare them with the final SIF runtime.

Apply this decision before troubleshooting packages: first classify the
resulting binary as `host-runtime` or `container-runtime`. If it is
`container-runtime`, a missing host `Python.h` is evidence that the build was
started on the wrong side of the boundary. It is not a missing host dependency.
Stop immediately and move the build into the candidate definition/builder;
do not spend time modifying the host toolchain.

Treat all of the following as `WRONG_BUILD_BOUNDARY` and stop before compiling:

- installing host `pythonX.Y-dev` to satisfy a container build;
- changing or replacing the host system Python for the SIF;
- using host virtual environments, site-packages, `/usr/local` libraries, or
  Python headers to build a container extension; or
- copying or bind-mounting a host-built `.so` into the candidate SIF.

The correct repair is always in the sealed SIF definition or ABI-identical
builder stage. A matching `cpXY` filename or a successful host import is not
evidence of container ABI compatibility. Provenance is mandatory: even if a
host-built extension later imports inside the SIF, it remains ineligible for
promotion because it bypassed the sealed build boundary.

A qualified base SIF is a dependency base, not an application-output source.
Its existing Provider, `libndn-service-framework.so*`, and `_ndnsf*.so` are
always treated as stale. The final stage must delete them before installing one
complete output set from the same container builder stage, require exactly one
active `_ndnsf*.so`, and compare all three hashes with
`container-native-build.json`. The executable definition validator performs
this check before Apptainer starts; deleted host input paths do not make an old
definition eligible again.

The exact-SIF test driver is subject to the same boundary. Before it invokes
`apptainer exec`, it may parse manifests and logs with pure Python, but it must
not import the host checkout's `ndnsf._ndnsf`, directly or through a MiniNDN
runner. All native NDNSF imports used by the workload occur inside the exact
candidate SIF.

The older reusable ML → stable NDN → mutable App OCI build is retained for
Spec158/local-development provenance only. It is not the Spec170 TigerCluster
release path. If that historical workflow is needed, see
[docs/layered-build.md](docs/layered-build.md) and do not copy its Docker/OCI
commands into a Spec170 run.

For a current Spec170 release, use
[`specs/170-reusable-layer-artifacts/quickstart.md`](../../specs/170-reusable-layer-artifacts/quickstart.md)
as the single operator checklist: seal source, build one complete SIF locally,
verify it locally, promote one hash-bound copy, then let Tiger verify and run
that copy. There is no `ndnsf-di-deploy install` or Tiger-side materialization
step in this route.

For iTiger Qwen work, start with the
[end-to-end operations runbook](docs/itiger-qwen-models.md), then apply the
[evidence and acceptance rules](docs/itiger-qwen-evidence.md). The current
runbook starts from the locally built SIF, then performs local closure checks,
project-storage promotion, and bounded Slurm execution. Older Docker security
smokes and Tiger-side OCI materialization are explicitly historical diagnostics
and are not interchangeable with the current SIF release.

Run the offline contract suite from the repository root:

```bash
tests/container/run.sh
```
