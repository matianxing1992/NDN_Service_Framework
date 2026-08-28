# Tasks: Layered Reusable NDNSF-DI Docker

## Phase 1: Setup and implementation gate

- [x] T001 Freeze the current image inventory, disk reserve, accepted Spec 110 rollback identity, layer ownership, and no-publish/no-iTiger boundaries in `specs/158-layered-reusable-docker/evidence/pre-implementation-inventory.md`, then pass strict structure, cross-artifact, and code-aware pre-implementation audit.

## Phase 2: Foundational lock and seal boundary

- [x] T002 Establish separate platform, ML, stable NDN, and mutable App locks plus layer-specific source sealing, mutation tests, and fail-closed verification in `packaging/ndnsf-di-container/oci/layered/locks/`, `packaging/ndnsf-di-container/oci/layered/scripts/prepare-layer-seals.py`, and `tests/container/layered/`.

## Phase 3: User Story 1 - Stable reusable foundations (P1)

**Independent test**: Cold-build four foundation images, run layer probes, and
record immutable identities and ownership exclusions.

- [x] T003 [US1] Implement and validate pinned ML development/runtime images with exact Python, PyTorch, ONNX Runtime GPU Python/C++ SDK, Transformers, closure, and no-model/no-secret gates in `packaging/ndnsf-di-container/oci/layered/Dockerfile.ml` and `tests/container/layered/`.
- [x] T004 [US1] Implement and validate stable NDN development/runtime images containing ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE, and websocketpp while proving ndn-svs, NDNSD, and NDNSF absent in `packaging/ndnsf-di-container/oci/layered/Dockerfile.ndn` and `tests/container/layered/`.

## Phase 4: User Story 2 - Mutable application image (P1)

**Independent test**: Build and probe the App image, then rebuild App under a
new identity with unchanged foundation IDs and no base compilation.

- [x] T005 [US2] Implement the mutable ndn-svs/NDNSD/NDNSF/NDNSF-DI builder in dependency order and its unprivileged runtime, including separate `/opt/ndnsf-app` ownership, C++/Python/native-provider closure, content scan, and static probe in `packaging/ndnsf-di-container/oci/layered/Dockerfile.app` and `tests/container/layered/`.
- [x] T006 [US2] Implement the single build driver and machine-readable lineage/evidence manifest, execute a complete cold build followed by an App-only rebuild, and retain immutable-ID/cache/probe evidence in `packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.sh` and `results/spec158-layered-reusable-docker/`.

## Phase 5: User Story 3 - Operations and safe reuse (P2)

**Independent test**: A second shell can inspect, rebuild one layer, and dry-run
cleanup without deleting accepted or remote evidence.

- [x] T007 [US3] Document routine App rebuild, foundation refresh, disk planning, lineage inspection, failure recovery, explicit cleanup, future registry cache, and OCI-to-SIF boundaries in `packaging/ndnsf-di-container/docs/layered-build.md` and synchronize the package entrypoint in `packaging/ndnsf-di-container/README.md`.

## Phase 6: Completion and convergence

- [x] T008 Reconcile FR/SC/task/test/evidence coverage, run focused/full container tests plus post-implementation Spec Kit audit and convergence, preserve limitations, ring the completion bell, and write the final verified outcome in `specs/158-layered-reusable-docker/completion-summary.md`.

## Dependencies

1. T001 blocks implementation.
2. T002 blocks every Dockerfile because locks and seals define their inputs.
3. T003 blocks T004; the NDN development/runtime products inherit ML products.
4. T004 blocks T005.
5. T005 blocks the complete and incremental builds in T006.
6. T006 provides measured commands and identities required by T007 and T008.

## Cohesion review

Each task closes one independently reviewable outcome. Tests, implementation,
focused validation, and evidence for the same layer remain together rather than
being split into mechanical file-operation tasks.
