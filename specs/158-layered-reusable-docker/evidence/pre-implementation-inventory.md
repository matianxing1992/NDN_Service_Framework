# Pre-implementation Inventory

**Captured**: 2026-07-26  
**Authority**: local inspection only

## Host and storage

- Root filesystem: 177 GB total, 129 GB used, 40 GB available (77% used).
- BuildKit cache reported 0 bytes.
- Build parallelism for this four-core host is capped at two by default.
- The host has no GPU acceptance authority; all local GPU checks are static.

## Protected rollback images

| Role | Reference | Image ID | Size | Policy |
|---|---|---|---:|---|
| Accepted Spec 110 runtime | `ndnsf-di:spec110-local-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0` | `sha256:57f3804cf787bec41d69f4dadcf286d030419b3cbd247678c92e0d2313829b6a` | 8,688,519,721 B | Protect until Spec 158 passes |
| Same accepted remote content | `ghcr.io/matianxing1992/ndnsf-di@sha256:dddb41e5c89cc8f24fe1cdba250c0dd675f244a9f0cdd64a99bc34d48cd4cf2e` | same local image ID | shared | Never delete remotely here |
| Current local foundation | `ndnsf-di-foundation:spec110-local-228341e0ea1f28956015fbaa30d2bf58a56b7789` | `sha256:a07c4470ed89b1d7da8fd626f94c4dc9062a2125dc969242a4588d5d87cf7158` | 360,566,424 B | Protect through migration |

## Confirmed rebuildable cleanup candidates

- `sha256:a15613e977bd...`: superseded Spec 110 foundation builder,
  approximately 5.4 GB.
- `sha256:5a42b06a4ab4...`: superseded Spec 110 foundation builder,
  approximately 5.4 GB.
- `sha256:3882ca6519f9...`: superseded local foundation, approximately
  361 MB.

These are candidates only. Cleanup remains an explicit image-ID operation and
must not use broad `docker system prune -a`.

## Existing graph defect being corrected

- `Dockerfile.foundation` installs ndn-svs and NDNSF into the supposedly stable
  foundation.
- `Dockerfile.gpu` installs the ML closure and rebuilds NDNSF in the final GPU
  assembly.
- Therefore an NDNSF/NDNSF-DI development change cannot be isolated from the
  expensive foundation/GPU build graph.

## Frozen and external boundaries

- Spec 110 Dockerfiles, task history, and accepted evidence are inputs and
  rollback only; no formal matrix is rerun.
- No GitHub workflow dispatch, image push, remote deletion, SIF
  materialization, or Slurm submission is authorized by Spec 158.
- Models and private identities remain runtime mounts outside OCI layers.
