# Docker overlay cleanup — 2026-08-16 (historical)

This is a closed cleanup record. It predates the 2026-08-17 local GPU SIF
candidate; its removed containerized Apptainer probe and retained remote SIF
are not release inputs for the current workflow.

## Scope

This cleanup removed only Docker overlay layers that were not referenced by
any remaining Docker image or container.  It did not touch the NDNSF source,
the current build tree, canonical `results/` evidence, TigerCluster files, or
the remote SIF.

## Before/after evidence

- Before: `/dev/sda5` had approximately 70 GiB available.
- Docker's normal `docker system prune -af` reclaimed an unreferenced
  containerized Apptainer probe. That probe was later removed permanently;
  final SIF construction uses host Apptainer 1.3.4.
- A daemon-state audit found 284 overlay directories not used by the only
  running container (`buildx_buildkit_multiarch`) or the remaining
  `moby/buildkit:buildx-stable-1` image.  Their measured payload was
  23,877,656,687 bytes (about 22.2 GiB).
- The Docker daemon was stopped, those exact unreferenced directories and
  their stale layer metadata/link entries were removed, and the daemon was
  restarted.
- After cleanup: `/dev/sda5` reports 87–88 GiB available (49% used), and
  `/var/lib/docker/overlay2` is about 383 MB.

## Post-cleanup validation

```text
docker ps: buildx_buildkit_multiarch Up
docker buildx ls: default builder running, BuildKit v0.13.2
host apptainer version: 1.3.4
tiger apptainer version: 1.3.4-1.el9
containerized alternate-version Apptainer probe: removed
```

At that time the remote Spec 170 SIF was not copied locally. The historical
The containerized alternate-version probe was removed and is never a release input. Current
SIF construction uses the sealed source and host Apptainer 1.3.4; no 80-GB
SIF is expected.
