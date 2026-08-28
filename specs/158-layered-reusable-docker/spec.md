# Feature Specification: Layered Reusable NDNSF-DI Docker

**Feature Directory**: `158-layered-reusable-docker`  
**Created**: 2026-07-26  
**Status**: Complete (local no-GPU acceptance)  
**Input**: Document the Docker configuration and build it step by step until
NDNSF-DI has a genuinely reusable image whose frequently changed application
code can be rebuilt without repeating the stable CUDA, ML, NDN, and security
stack.

## Scope and authority

This feature owns a new local, layered OCI build path for the current
NDNSF-DI GPU runtime. It does not modify or invalidate frozen Spec 110 evidence,
does not delete the accepted Spec 110 rollback image before replacement
validation passes, does not publish images, does not submit iTiger jobs, and
does not place model weights or deployment credentials in an image.

The feature separates stable machine-learning dependencies, stable NDN and
security dependencies, and frequently changed application sources. The
existing Spec 110 Dockerfiles remain a rollback path until the new final image
passes its local acceptance contract.

## User Scenarios & Testing

### User Story 1 - Build stable reusable foundations once (Priority: P1)

As an NDNSF developer, I can build checksum-bound ML and NDN foundation images
once and reuse their immutable identities for later application builds.

**Why this priority**: CUDA, Python ML packages, NFD, ndn-cxx, and the security
libraries are the expensive dependencies that rarely change.

**Independent Test**: Starting from an empty feature-specific BuildKit cache,
build and inspect the ML development/runtime and NDN development/runtime
images. Their manifests identify exact inputs, contain no application source,
model, or credential, and pass their layer-specific probes.

**Acceptance Scenarios**:

1. **Given** pinned upstream images and dependency locks, **When** the stable
   foundations are built, **Then** every produced image has a recorded image ID,
   configuration digest, build duration, size, and input-lock digest.
2. **Given** an ML runtime image, **When** it is probed, **Then** Python,
   PyTorch, ONNX Runtime GPU, Transformers, and the ONNX Runtime C++ SDK are
   importable or present at their pinned versions without model weights.
3. **Given** an NDN runtime image, **When** it is probed, **Then** NFD,
   ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE, and their runtime library closure are
   present; ndn-svs, NDNSD, and NDNSF are absent.
4. **Given** any source seal, **When** its digest, revision, archive content, or
   layer ownership is inconsistent, **Then** the build fails before producing
   an accepted image.

---

### User Story 2 - Rebuild only the mutable application (Priority: P1)

As an NDNSF developer, I can change ndn-svs, NDNSF, or NDNSF-DI and rebuild a
working final image without rebuilding the stable ML or NDN foundations.

**Why this priority**: This is the requested reuse benefit and the main reason
for replacing the monolithic build graph.

**Independent Test**: Build the application image, repeat the build with a new
application build identity, and prove that both builds reference identical ML
and NDN base image IDs while only application stages execute again.

**Acceptance Scenarios**:

1. **Given** accepted ML and NDN foundations, **When** the application image is
   built, **Then** ndn-svs is built before its NDNSD dependent and NDNSF, the
   C++ and Python bindings are installed, the native ONNX provider is linked to
   the pinned SDK, and the static runtime probe passes as an unprivileged user.
2. **Given** an application-only change, **When** the application build is
   repeated, **Then** the stable foundation image IDs remain byte-for-byte
   unchanged and no ML or stable NDN compilation step runs.
3. **Given** the final image, **When** it is scanned, **Then** no model weights,
   private keys, tokens, passwords, source-control metadata, compiler cache, or
   dependency source tree is present.
4. **Given** a missing or incompatible application dependency, **When** the
   final closure check runs, **Then** the image is rejected rather than falling
   back silently to CPU or host libraries.

---

### User Story 3 - Operate, inspect, and roll back safely (Priority: P2)

As an operator, I have one documented command surface to build selected layers,
inspect lineage, reproduce a final image, and clean only superseded local
artifacts.

**Independent Test**: Follow the quickstart on the current host, build the
complete local image, generate a machine-readable manifest, run the probes,
and execute a dry-run cleanup that protects the accepted legacy and current
images.

**Acceptance Scenarios**:

1. **Given** a fresh shell, **When** the operator follows the guide, **Then**
   layer order, inputs, tags, expected duration, disk requirements, verification,
   and cleanup policy are explicit.
2. **Given** a completed build, **When** its manifest is inspected, **Then** the
   full parent chain and all lock/source digests are recoverable without relying
   on Docker's mutable tag names.
3. **Given** a failed replacement build, **When** cleanup is considered,
   **Then** the accepted Spec 110 local image remains available.
4. **Given** a successful replacement, **When** cleanup runs, **Then** only
   explicitly enumerated, unreferenced, reproducible local images are removed;
   remote GHCR evidence is untouched.

## Edge Cases

- The host has no NVIDIA GPU, so build/static closure can pass while live CUDA
  execution remains explicitly unverified.
- A pinned external archive is temporarily unavailable.
- The repository is dirty and cannot represent a formal immutable release.
- A base image tag exists but resolves to a different digest.
- A build is interrupted after one foundation succeeds.
- Disk space becomes insufficient during a cold build.
- Multiple tags reference the same image ID.
- BuildKit cache is empty even though reusable foundation images exist.

## Functional Requirements

- **FR-001**: The build SHALL expose separate ML development/runtime, NDN
  development/runtime, and application runtime products.
- **FR-002**: The ML layer SHALL own CUDA/cuDNN user-space, Python 3.10,
  PyTorch, ONNX Runtime GPU (Python and C++ SDK), Transformers, and their
  verified runtime closure.
- **FR-003**: The stable NDN layer SHALL own ndn-cxx, NFD, OpenABE/RELIC,
  NAC-ABE, and websocketpp, and SHALL exclude ndn-svs, its NDNSD dependent,
  NDNSF, NDNSF-DI, models, and deployment credentials.
- **FR-004**: The application layer SHALL build ndn-svs, then NDNSD, then own
  NDNSF Core, Python bindings, NDNSF-DI packages, examples, and native
  inference adapters.
- **FR-005**: Each layer SHALL use a separate lock and source-seal boundary so
  an application revision cannot invalidate stable foundation locks.
- **FR-006**: All upstream base images, source revisions, and downloaded
  archives SHALL be digest- or checksum-bound and verified before use.
- **FR-007**: Compiled stable and mutable prefixes SHALL remain distinct so
  ownership and runtime closure can be audited.
- **FR-008**: The operator SHALL be able to build all layers or one selected
  layer through one documented command surface.
- **FR-009**: A machine-readable build manifest SHALL record commands, input
  digests, image IDs/digests, parent identities, timestamps, durations, sizes,
  and verification results.
- **FR-010**: The build SHALL fail closed on missing source seals, lock drift,
  unsafe archives, unresolved required libraries, version mismatch, or
  unrequested CPU fallback.
- **FR-011**: The final runtime SHALL run as an unprivileged user with a
  read-only-root-compatible layout and retain the existing static health probe.
- **FR-012**: The image SHALL exclude model weights, private identities,
  credentials, tokens, host caches, Git metadata, results, and build-only
  source trees.
- **FR-013**: An application-only rebuild SHALL reuse the exact accepted ML and
  stable NDN image IDs and SHALL not execute their compilation stages; NDNSD
  SHALL rebuild with the mutable ndn-svs application dependency.
- **FR-014**: The build SHALL preserve the accepted Spec 110 image as a local
  rollback until all new local gates pass.
- **FR-015**: Cleanup SHALL be explicit and image-ID based; broad destructive
  pruning and deletion of remote evidence are prohibited.
- **FR-016**: Local no-GPU verification SHALL not be represented as live CUDA or
  iTiger acceptance.
- **FR-017**: The old Spec 110 Dockerfiles and evidence SHALL not be rewritten
  or rerun by this feature.
- **FR-018**: Build documentation SHALL distinguish routine application rebuild,
  foundation refresh, formal release, and later OCI-to-SIF materialization.

## Assumptions

- The existing pinned Ubuntu 20.04, CUDA 12.4.1/cuDNN 9, Python 3.10,
  ONNX Runtime 1.20.1, and PyTorch 2.6.0/cu124 closure remains the compatibility
  baseline for this migration.
- The first completed image is a local development candidate because the
  current workspace may contain uncommitted user changes.
- Live GPU execution and iTiger SIF acceptance remain later explicit gates and
  are not silently substituted by local static probes.

## Success Criteria

- **SC-001**: A cold local run produces all five named image products and a
  complete build manifest without embedding models or credentials.
- **SC-002**: All layer-specific probes and the final unprivileged static
  runtime probe pass.
- **SC-003**: A second application build with a distinct application identity
  keeps all four foundation image IDs unchanged and reports zero executed ML or
  stable NDN compilation steps.
- **SC-004**: The application-only rebuild completes without requiring another
  download or compilation of CUDA, PyTorch, ONNX Runtime, NFD, ndn-cxx,
  OpenABE, or NAC-ABE.
- **SC-005**: A new operator can reproduce the local candidate and identify
  exactly which input change requires rebuilding each layer by following the
  maintained guide.
- **SC-006**: Failure or interruption at any layer leaves previously accepted
  foundation and legacy runtime images usable.
- **SC-007**: The final audit finds no unresolved CRITICAL or HIGH issue in
  requirements, ownership, security, rollback, source sealing, or evidence.
