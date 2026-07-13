# NDNSF-DI OCI Container Package

This package is the implementation surface for Spec 108. It produces one
digest-bound OCI release and executes it through two thin adapters:

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
oci/            OCI build source (added in Phase 3)
adapters/       runtime templates (added by their story phases)
```

Run the offline contract suite from the repository root:

```bash
tests/container/run.sh
```
