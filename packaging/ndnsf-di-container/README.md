# NDNSF-DI OCI Container Package

This package is the implementation surface for the NDNSF-DI container release.
It supports the existing digest-bound release and a reusable layered local
development build. Runtime execution uses two thin adapters:

- `docker-compose` for long-lived cloud hosts; and
- `slurm-apptainer` for bounded iTiger allocations.

The package owns build, materialization, lifecycle integration, profile
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
oci/            OCI build sources, including the layered local build
adapters/       runtime templates (added by their story phases)
```

For the reusable ML → stable NDN → mutable App build, including routine App
rebuild, evidence, failure recovery, cleanup, and future iTiger boundaries, see
[docs/layered-build.md](docs/layered-build.md).

For iTiger Qwen work, start with the
[end-to-end operations runbook](docs/itiger-qwen-models.md), then apply the
[evidence and acceptance rules](docs/itiger-qwen-evidence.md). The runbook
separates local Docker security smoke, Slurm/SIF validation, allocation-scratch
capacity, cross-node NFD probing, model preparation, full generation, and
formal repeated campaigns so failures are found at the cheapest valid gate.

Run the offline contract suite from the repository root:

```bash
tests/container/run.sh
```
