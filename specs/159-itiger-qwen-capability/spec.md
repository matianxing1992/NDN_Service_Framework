# Feature Specification: iTiger NDNSF-DI Qwen Capability

**Feature Directory**: `159-itiger-qwen-capability`  
**Created**: 2026-07-27  
**Status**: In progress  
**Input**: Use the accepted Spec 158 layered Docker candidate on TigerCluster
to verify real GPU-backed Qwen inference through NDNSF-DI.

## Scope and authority

This feature validates one development candidate, not a formal production
release. It does not modify or relabel frozen Spec 110 failures or Spec 158
local evidence. It may publish a uniquely named capability candidate, create
one checksum-bound SIF, and submit bounded Slurm jobs authorized by the user.
Every submitted identity is exactly-once; a measured failure is preserved
rather than silently retried.

## User Scenarios & Testing

### User Story 1 - Materialize the exact candidate (Priority: P1)

As an operator, I can identify one immutable OCI digest for the locally accepted
Spec 158 App image and materialize exactly that digest as a verified SIF in
project storage.

**Independent Test**: The promoted SIF record binds the local image ID, OCI
digest, SIF SHA-256, build identity, Slurm job ID, node, and terminal state.

### User Story 2 - Prove GPU Qwen execution (Priority: P1)

As a researcher, I can run the frozen Qwen2.5-0.5B model inside the exact SIF
on an allocated GPU and receive deterministic standalone inference without CPU
fallback.

**Independent Test**: The allocation records the GPU UUID, container-visible
CUDA, PyTorch CUDA operation, ONNX Runtime CUDA provider where used, model
revision, prompt, and generated tokens.

### User Story 3 - Prove real NDNSF-DI inference (Priority: P1)

As a researcher, I can send a normal secured NDNSF-DI request to a Qwen
provider and receive the model output through the real request/response path.

**Independent Test**: NFD, controller, requester, and provider readiness are
recorded; permission, token, selection, provider execution, backend, and final
response evidence correlate to one request ID.

## Functional Requirements

- **FR-001**: The candidate SHALL be the exact locally accepted
  `ndnsf-di:spec158-app-reuse-proof` image ID and SHALL receive a unique,
  non-overwriting GHCR reference.
- **FR-002**: Acceptance SHALL bind an immutable OCI digest and SIF SHA-256;
  mutable tags alone are insufficient.
- **FR-003**: OCI-to-SIF work SHALL run in a bounded CPU Slurm allocation, not
  on the login node, and SHALL promote only a verified artifact and evidence.
- **FR-004**: GPU work SHALL run through Slurm and `apptainer exec --nv`;
  login-node or host-only GPU observations are insufficient.
- **FR-005**: The model SHALL be `Qwen/Qwen2.5-0.5B-Instruct` revision
  `7ae557604adf67be50417f59c2c2f167def9a775`, mounted read-only from project
  storage and excluded from OCI/SIF.
- **FR-006**: Missing CUDA, a mismatched model, or unrequested CPU fallback
  SHALL fail the GPU gate.
- **FR-007**: Standalone inference SHALL pass before the NDNSF-DI candidate is
  submitted.
- **FR-008**: NDNSF-DI validation SHALL use the normal secured dynamic runtime
  path without authorization, token, selection, or backend bypasses.
- **FR-009**: Evidence SHALL correlate OCI/SIF/model/GPU/job/request identities,
  generated tokens, timing, logs, and terminal status.
- **FR-010**: Each formal submission identity SHALL be submitted at most once;
  post-start failures SHALL be preserved and any replacement requires a new
  linked identity.
- **FR-011**: Credentials, identities, and tokens SHALL not enter images,
  manifests, logs, or repository artifacts.
- **FR-012**: This capability result SHALL not claim formal release,
  multi-node, multi-GPU, performance, or physical-production acceptance.

## Edge Cases

- Compute-node UID resolution differs by node.
- GHCR is reachable from the workstation but not a compute node.
- Apptainer conversion succeeds but GPU driver libraries do not bind.
- The model directory exists but is incomplete or does not match its revision.
- Standalone inference succeeds while the secured NDNSF-DI path fails.
- The job starts and fails before durable evidence promotion.

## Success Criteria

- **SC-001**: One immutable OCI digest is published and one matching SIF digest
  is promoted with a PASS materialization record.
- **SC-002**: One allocated GPU executes the frozen model and produces the
  expected non-empty standalone output with no CPU fallback.
- **SC-003**: One real NDNSF-DI request returns Qwen-generated content with
  correlated security, provider, backend, and response evidence.
- **SC-004**: Every submitted job has one immutable identity, one job ID, one
  terminal outcome, and durable logs.
- **SC-005**: Final reporting explicitly preserves all failures and authority
  limitations.

## Assumptions

- The current VPN and SSH session remain available.
- A development-candidate GHCR publication is acceptable for this bounded
  capability test.
- The existing sealed Qwen model under project storage is complete; this must
  be verified before GPU submission.
