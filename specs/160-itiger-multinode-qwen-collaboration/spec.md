# Feature Specification: iTiger Multi-Node Qwen Collaboration

**Feature Directory**: `160-itiger-multinode-qwen-collaboration`  
**Created**: 2026-07-27  
**Status**: Complete  
**Input**: Use multiple TigerCluster nodes to collaboratively complete one
NDNSF-DI Qwen inference request.

## Scope and authority

This feature validates one cross-node development capability. It reuses the
frozen Qwen revision and accepted Spec 159 evidence where still applicable, but
the live runtime must be a single coherent container image. A live inference
attempt must not combine an older SIF with externally mounted replacement C++
libraries, Python extension modules, or ad-hoc Python dependency directories.
It proves layer-pipeline collaboration, not tensor parallelism, replicated
serving, throughput scaling, production readiness, or a formal release. Every
submitted Slurm identity is exactly-once; measured failures are preserved and
are never silently retried.

## User Scenarios & Testing

### User Story 1 - Qualify cross-node NDN transport (Priority: P1)

As an operator, I can allocate three physical GPU nodes and prove that
allocation-local NFD instances can exchange named Data over explicit inter-node
faces before any model execution is attempted.

**Independent Test**: The probe records three distinct hostnames, allocated GPU
UUIDs, NFD endpoints/faces/routes, a unique probe Data name, producer node,
consumer node, payload digest, RTT, and terminal status.

### User Story 2 - Collaboratively execute one Qwen request (Priority: P1)

As a researcher, I can send one secured NDNSF-DI request whose three Qwen layer
stages execute on three distinct physical nodes and whose intermediate hidden
states cross the two node boundaries before the requester receives the final
answer.

**Independent Test**: One request/session record correlates Stage 0, Stage 1,
and Stage 2 with distinct nodes and GPU UUIDs, exact layer ranges, two
cross-node dependency Data names and digests, and one requester-visible final
response matching the frozen reference.

### User Story 2a - Rebuild a coherent runtime after mixed-runtime failure (Priority: P1)

As an operator, I can reject the current mixed runtime after native constructor
crashes and rebuild a clean NDNSF-DI Qwen runtime image before attempting
another live collaboration request.

**Independent Test**: Inside the rebuilt image, without externally mounted
replacement `.so`, Python extension, or vendor-site directories, a bounded
Slurm smoke constructs `ServiceController`, `ServiceProvider`, and
`ServiceUser`, imports Qwen runtime dependencies, and runs a single-node
NDNSF-DI fake or Qwen smoke without allocator corruption.

### User Story 3 - Obtain auditable capability evidence (Priority: P2)

As an operator, I can determine exactly what was allocated, executed,
transferred, and returned without relying on provider-ready messages alone.

**Independent Test**: Durable evidence binds the SIF, model, stage artifacts,
policy, source bundle, Slurm allocation, NFD topology, request/session,
per-stage backend, dependency objects, response, and terminal state.

## Functional Requirements

- **FR-001**: The acceptance run SHALL allocate exactly three distinct physical
  TigerCluster nodes, one GPU per node, and assign exactly one Qwen stage to
  each node.
- **FR-002**: Cross-node model work SHALL remain disabled until an
  allocation-scoped NFD TCP/UDP Data exchange probe passes on those nodes.
- **FR-003**: The model SHALL be `Qwen/Qwen2.5-0.5B-Instruct` revision
  `7ae557604adf67be50417f59c2c2f167def9a775`; stage ranges SHALL be `[0,8)`,
  `[8,16)`, and `[16,24)`.
- **FR-004**: All three stages SHALL use an allocated GPU and SHALL fail closed
  if CUDA is unavailable or any stage falls back to CPU.
- **FR-005**: The run SHALL use the existing NDNSF-DI collaboration request,
  role assignment, planned dependency Data, segmented transfer, and final
  response path without service-, Qwen-, node-, or workload-specific Core
  bypasses.
- **FR-006**: Stage 0 output SHALL be fetched by Stage 1 across one physical
  node boundary, and Stage 1 output SHALL be fetched by Stage 2 across a second
  physical node boundary.
- **FR-007**: The requester SHALL receive the final output through the normal
  secured REQUEST, ACK, SELECTION, collaboration execution, and RESPONSE flow.
- **FR-008**: Acceptance SHALL correlate request ID, session ID, role,
  provider, hostname, GPU UUID, layer range, backend, planned Data name,
  payload digest/bytes/segments, and timestamps.
- **FR-009**: The accepted Spec 159 SIF and frozen model SHALL be mounted
  read-only only until live Attempt 005 and its diagnostics are recorded as the
  mixed-runtime baseline failure. Any replacement live attempt SHALL use a
  rebuilt coherent SIF/image whose C++ libraries, Python bindings, Python
  packages, and NDNSF-DI source were built or installed together inside the
  same image contract.
- **FR-010**: All compute, model preparation, NFD, and inference work SHALL run
  under bounded Slurm allocations, never on the login node.
- **FR-011**: Each probe or inference submission identity SHALL be submitted at
  most once. A started failure SHALL remain evidence; a replacement requires a
  new, explicitly linked identity and revised task state.
- **FR-012**: No password, token, persistent private key, or session credential
  SHALL enter the SIF, stage artifacts, logs, or durable evidence.
- **FR-013**: Final reporting SHALL explicitly distinguish layer-pipeline
  collaboration from tensor/model parallelism and make no performance or
  scalability claim from a single capability run.
- **FR-014**: A formal live NDNSF-DI Qwen attempt SHALL NOT mount externally
  built `libndn-service-framework.so`, `_ndnsf*.so`, `_py_repoclient*.so`, or
  ad-hoc `vendor-site` Python packages over an older runtime image. It may bind
  only read-only model/stage artifacts, policy/config inputs, job scripts, and
  writable evidence directories.
- **FR-015**: The mixed-runtime failures from live Attempt 005 and provider
  constructor diagnostics SHALL be preserved as negative evidence and SHALL NOT
  be relabeled as Qwen model, algorithm, or distributed-inference logic
  failures; they failed before model preload and before request execution.

## Edge Cases

- Slurm grants fewer than three distinct nodes or a node lacks the requested GPU.
- Hostnames are distinct but the job steps accidentally launch on one node.
- NFD processes start but inter-node faces/routes do not carry Data.
- Providers advertise roles but the planner assigns two roles to one provider.
- A stage loads on CPU despite a visible GPU.
- A native NDNSF object fails during construction before Qwen model preload,
  indicating runtime/linkage inconsistency rather than Qwen execution failure.
- A live harness attempts to repair the runtime by mounting local build outputs
  into an older SIF instead of rebuilding a coherent image.
- Stage 0 publishes an object that Stage 1 fetches locally or from the wrong
  producer.
- A dependency object is published but its digest, segment count, or request
  identity does not match the plan.
- Stage 2 computes but the final secured response never reaches the requester.

## Success Criteria

- **SC-001**: One exactly-once allocation probe proves named Data exchange
  among three distinct allocated GPU nodes.
- **SC-002**: One exactly-once NDNSF-DI request executes the three frozen Qwen
  layer ranges on three distinct GPU UUIDs with zero CPU fallback.
- **SC-003**: Both planned hidden-state objects are fetched across physical
  node boundaries with matching producer/consumer identities and SHA-256.
- **SC-004**: The requester receives a final response whose top token and shape
  match the frozen full-model reference.
- **SC-005**: Durable evidence is complete enough to reject same-node,
  replicated-request, provider-only, CPU-only, and uncorrelated-output false
  positives.
- **SC-006**: Before any replacement live Qwen attempt after Attempt 005, the
  rebuilt runtime image passes native-constructor and single-node NDNSF-DI
  smoke gates inside Slurm without allocator corruption or externally mounted
  runtime overrides.

## Assumptions

- The live account continues to allow three concurrent nodes and three GPUs.
- The accepted Spec 159 SIF remains readable from project storage.
- The existing three-stage Qwen pipeline is the reference implementation; a
  new distributed-inference algorithm is out of scope.
