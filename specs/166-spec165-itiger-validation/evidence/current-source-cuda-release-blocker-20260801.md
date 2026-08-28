# Current-Source CUDA Candidate Release Blocker — 2026-08-01

## Finding

The generic TigerCluster GPU capability probe passed, but a current-source
CUDA candidate cannot be promoted from this checkout yet. The local Docker
build graph itself passes its static preflight; the release inputs are not
currently identity-safe or storage-safe for a full foundation/GPU build.

## Evidence

- Current repository `HEAD`: `f179f779f2e6863f23d8387d38f6b45bb58a59ef`.
- The checkout has 129 tracked modifications. A sealed release must bind a
  clean, immutable source revision; the current working tree is not that
  revision.
- Existing `.spec110-build/source-seal.json` binds the older workspace revision
  `edeeff1e3041e941b48ba18784348fc9505d7418`.
- Existing seal lock digest:
  `sha256:0b617bb8a463734c837422786178e455abc327150d6cdbd0eccff16b6460312a`.
- Current `packaging/ndnsf-di-container/oci/locks/gpu.lock` digest:
  `sha256:06a269c56ff651a1a9e55f7fa3f7d893f8d4fa8ff619832e818bb9c5784bc693`.
- `prepare-sealed-context.py verify` correctly rejects the stale seal with
  `SOURCE_SEAL_LOCK_MISMATCH`.
- `preflight-gpu-build.py --workspace .` passes the static graph checks, but it
  does not create a foundation or GPU image.
- Local root filesystem has only about 17 GiB available; a complete foundation
  plus CUDA build must not be started under this capacity.
- The only promoted TigerCluster SIF remains the older Spec166 candidate
  (`dcef2858…` image, `e82d5d4b…` SIF), already recorded as identity-incompatible
  with the current local CPU-gate source/workload closure.

## Decision

No local foundation/GPU build and no candidate-bound standalone job were
started. Reusing the old SIF or treating a temporary dirty-tree image as a
current release would break source/image reproducibility.

## Required next release action

Create an immutable clean source revision containing the accepted current code,
regenerate the sealed dependency context and foundation image, build the CUDA
GPU image through the pinned release path, publish and materialize its exact
OCI/SIF digests, then run the single-GPU standalone preflight before any
three-node NDNSF-DI workload.
