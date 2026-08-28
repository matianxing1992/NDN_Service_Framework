# Quickstart Validation

## Prerequisites

- Docker Buildx/BuildKit is running.
- At least 35 GB of free disk remains after preserving the accepted Spec 110
  rollback image.
- Network access is available for the first cold ML build.
- No local GPU is required for build and static checks.

## Planned command

```bash
packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.sh \
  --target all \
  --jobs 2 \
  --output results/spec158-layered-reusable-docker/<build-id>
```

## Expected products

```text
ndnsf-di-ml:*-(devel|runtime)
ndnsf-di-ndn:*-(devel|runtime)
ndnsf-di:spec158-*
```

The output directory must contain `build-manifest.json`, one log per layer,
probe results, and a secret/content scan.

## Incremental proof

Repeat only the App target with a different build identity:

```bash
packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.sh \
  --target app \
  --jobs 2 \
  --app-build-id reuse-proof \
  --output results/spec158-layered-reusable-docker/<reuse-build-id>
```

The second manifest must reference the same four foundation image IDs and show
no ML or stable NDN build.

## Evidence boundary

Passing this guide proves a reusable local OCI build and static runtime closure.
It does not prove live CUDA execution, OCI publication, SIF materialization, or
iTiger acceptance.
