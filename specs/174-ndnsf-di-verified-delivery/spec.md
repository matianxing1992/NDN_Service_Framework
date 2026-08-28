# Feature Specification: Verified NDNSF-DI Delivery

**Feature Branch**: Not created; this specification is independent of branch naming

**Created**: 2026-08-20

**Status**: Draft

**Input**: Converge the substantial existing NDNSF-DI implementation onto the design shown in `docs/NDNSFDI/slides/main.pdf`; reuse and repair the current code and tests instead of rebuilding them; prove the resulting path with unit, integration, and MiniNDN+CPU tests; and proceed to TigerCluster only after every local gate passes.

## Design Authority and Scope

The design authority for this feature is the 48-page PDF
`docs/NDNSFDI/slides/main.pdf`, SHA-256
`5a1096e5d9d5cf3899de7c305c7b98ea7981c56986118253d63feb91043bd668`.
The source code and tests determine what is currently implemented; the PDF
determines the target behavior.

This specification replaces Spec170 as the active authority for remaining
NDNSF-DI implementation and validation. Spec170 remains historical evidence and
may be consulted for reusable code or verified results, but its task count,
candidate-image history, and superseded design variants are not inherited.

### Existing Implementation Is The Baseline

Spec 174 is a convergence and verification feature, not a greenfield rewrite.
The repository already contains substantial native placement, plan sealing,
Provider execution, NDN dependency I/O, tensor/collective, protected-runtime,
Python SDK, MiniNDN, packaging, and TigerCluster work. That implementation is
the mandatory starting point.

Before adding or replacing production behavior, implementation work MUST map
the relevant current symbols, tests, and scripts to this specification and
classify each item as:

1. **conforming and reusable** — preserve it and add only missing proof;
2. **partially conforming** — repair the smallest coherent boundary and retain
   compatible behavior and tests;
3. **conflicting or obsolete** — isolate or remove it only with a named
   migration and regression proof; or
4. **unverified** — execute the appropriate existing test before assuming it
   passes or rewriting it.

The first reference surfaces include the current `CollaborationAckClosure`,
`PlacementPlanCoreV3`, `NativeExecutionPlan`, `NativeProviderHandler`,
`ProviderRoleWorker`, `NdnsfCollaborationDependencyIo`, `TensorBundleCodec`,
`CollectiveRuntime`, `ProviderGroupCoordinator`, and `ProtectedRuntime` native
components; the current Python `core`, `app_sdk`, planner, ONNX adapter, and
ONNX Runtime backend; the existing distributed-inference unit/integration
tests; the Spec170 Python regressions; the real MiniNDN scripts; and the local
SIF/Tiger preflight tooling. This list establishes reuse candidates, not a
claim that every candidate already satisfies the target.

A parallel Spec174-only runtime, planner, tensor transport, protected runtime,
or deployment pipeline MUST NOT be created when a conforming current owner can
be extended. A new production file or subsystem requires an explicit recorded
gap showing why no current owner can responsibly contain the behavior.

The feature is complete only through a strict sequence:

1. existing behavior inventoried, verified where possible, and only confirmed
   target gaps repaired and unit-tested;
2. full in-process integration lifecycle and packet faults passed;
3. real MiniNDN+CPU correctness passed;
4. the exact locally qualified deployment candidate passed TigerCluster
   qualification.

Failure at any gate stops promotion to the next gate. A lower gate cannot prove
a higher one, and a higher-cost run cannot compensate for a lower-gate failure.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Execute One Dynamically Placed Request (Priority: P1)

An application submits model identity, input, and policy intent without a
precomputed deployment. NDNSF-DI discovers willing Providers, closes the signed
offer set, chooses a legal split and one-to-one role assignment, sends each
Provider its exact Selection, and returns one complete application result.

**Why this priority**: This is the minimum end-to-end behavior that distinguishes
NDNSF-DI from a static deployment wrapper.

**Independent Test**: A four-role request can be executed by four Providers from
REQUEST through ACK closure, plan commit, Selection, role-local preparation,
dependency execution, and final RESPONSE without pre-registering a stage map.

**Acceptance Scenarios**:

1. **Given** four eligible Providers and a splittable model graph, **When** the application submits one request, **Then** ACK collection closes once, one immutable plan assigns every role to a distinct Provider, and exactly one complete response is accepted.
2. **Given** an assignment that reuses one Provider for two roles or spreads one logical role across Providers, **When** the plan is validated, **Then** it is rejected before Selection and no Provider begins work.
3. **Given** a Provider that offered capacity but cannot validate its final Selection, **When** Selection arrives, **Then** that Provider fails closed without executing the model or publishing downstream data.

---

### User Story 2 - Exchange Pipeline and Tensor Data Through NDN (Priority: P1)

Each selected Provider owns one complete execution role. Pipeline stages and
tensor-parallel ranks exchange authenticated intermediate tensors using
deterministic, producer-owned NDN names. Consumers know exactly which manifests
and segments to request and which complete input set must arrive before their
role can run.

**Why this priority**: Correct cross-Provider dataflow is the core distributed
execution mechanism; local ONNX execution alone does not demonstrate NDNSF-DI.

**Independent Test**: A pipeline, a two-rank tensor group, and a hybrid graph
complete using only planned NDN Interest/Data dependencies and match an unsplit
reference output.

**Acceptance Scenarios**:

1. **Given** a sealed producer/consumer edge, **When** the consumer requests the manifest and segments, **Then** the full names, producer, attempt, plan, epoch, operation, layout, and payload digests are verified before the tensor becomes ready.
2. **Given** duplicate or out-of-order valid Data, **When** all required segments arrive, **Then** the object is reconstructed exactly once and execution proceeds once.
3. **Given** loss, replay, stale epoch, wrong rank, wrong layout, corruption, cancellation, or a permanently missing peer, **When** the dependency deadline is reached, **Then** the affected role or group fails boundedly and no partial final response is accepted.

---

### User Story 3 - Prove Correctness Locally Before Deployment (Priority: P1)

A developer can verify the implementation from cheap deterministic checks to a
real multi-process NDN environment without GPUs. The local result states which
gate passed, the exact first failure when a gate fails, and whether the code is
eligible for cluster deployment.

**Why this priority**: Most protocol, naming, state-machine, packaging, and
process-wiring errors should be found without consuming TigerCluster resources.

**Independent Test**: One documented local command sequence runs the relevant
unit suite, the integration suite, and MiniNDN+CPU cases from clean process
starts and produces a bounded evidence summary.

**Acceptance Scenarios**:

1. **Given** a code or contract defect, **When** unit or integration tests fail, **Then** MiniNDN and TigerCluster promotion are blocked.
2. **Given** all unit and integration tests pass, **When** MiniNDN runs the pipeline, tensor, and hybrid CPU cases, **Then** every final output matches the unsplit oracle and every planned cross-Provider edge is observed by exact name.
3. **Given** a MiniNDN failure, **When** the run terminates, **Then** evidence identifies the first missing or invalid lifecycle event or packet and no unbounded background campaign remains.
4. **Given** an existing NDNSF-DI module or regression that already implements the required behavior, **When** the feature is planned and implemented, **Then** that owner is reused and revalidated rather than replaced by a parallel Spec174-only implementation.

---

### User Story 4 - Qualify the Same Candidate on TigerCluster (Priority: P2)

After all local gates pass, an operator promotes the exact immutable deployment
candidate to TigerCluster, verifies its digest and runtime closure, and advances
through bounded CPU, single-GPU, and multi-Provider GPU checks before attempting
a complete large-model answer.

**Why this priority**: Cluster testing is necessary for physical GPU and
cross-node evidence, but only after the design is already correct locally.

**Independent Test**: The candidate that passed locally is hash-identical on
TigerCluster and completes the staged qualification without rebuilding or
substituting libraries on the cluster.

**Acceptance Scenarios**:

1. **Given** any failed or missing local gate, **When** TigerCluster submission is considered, **Then** submission is refused.
2. **Given** a locally qualified immutable candidate, **When** it is promoted, **Then** TigerCluster verifies the same digest before execution and records the allocated hardware and runtime providers.
3. **Given** a GPU-qualified candidate, **When** the multi-Provider case runs, **Then** each Provider owns one role or rank, cross-Provider tensors use the sealed NDN dataflow, CPU fallback is absent, and the complete answer matches the frozen reference semantics.

### Edge Cases

- No Provider returns a usable offer before ACK closure.
- The offer set is valid, but no legal one-to-one role assignment fits resource constraints.
- A Provider restarts after offering capacity, changing its boot or protection epoch.
- The model name matches but graph, initializer, adapter, precision, backend, or runtime identity differs.
- Selection is duplicated, delayed, replayed, or bound to another attempt or plan.
- A tensor manifest is valid but one segment never arrives.
- Data arrives from the wrong producer, rank, epoch, plan, signer, or full name.
- A tensor group loses one rank after other ranks become ready.
- Cancellation races with fetch, assembly, device admission, execution, or final response publication.
- A cached artifact or state entry is only partially compatible.
- MiniNDN processes terminate out of order or leave stale NFD, key, or runtime state.
- TigerCluster hardware, network, or storage is unavailable after all local gates pass.

## Requirements *(mandatory)*

### Functional Requirements

#### Request, Planning, and Ownership

- **FR-001**: The application request MUST contain model/task identity, input, deadline, policy, and acceptance intent, but MUST NOT require a Provider list, stage map, preloaded deployment, or precomputed split.
- **FR-002**: NDNSF-DI MUST collect signed, request-bound Provider offers and establish one immutable ACK-closure record before placement begins.
- **FR-003**: The final plan MUST assign every execution role exactly once to a distinct Provider; one Provider MUST NOT own two roles in the same attempt, and one logical role MUST NOT span Providers.
- **FR-004**: Tensor parallelism MUST represent each rank as its own execution role on one Provider; group membership and collective semantics belong to an explicit tensor-group contract.
- **FR-005**: The default policy MUST be ACK-driven pre-split-first placement. The policy extension MUST be a bounded, side-effect-free decision over sanitized snapshots; it receives no executable, network, storage, key, lease, or device authority, and its proposal is revalidated by the trusted core. Reuse may affect cost but MUST NOT create a separate default architecture or weaken correctness constraints.
- **FR-006**: Plan validation and commit MUST finish before any Selection is sent. The request and ACK offer permission to consider work; only the exact Provider-specific Selection grants execution authority.
- **FR-007**: Every Provider MUST validate its local role, artifact, dataflow, security, attempt, plan, and fresh resource bindings before preparation or execution. Capacity decisions MUST use a complete runtime peak vector rather than model file size alone and MUST be revalidated atomically before device admission.

#### Artifacts and Execution

- **FR-008**: Persistent model artifacts MUST be canonical, content-addressed ONNX graph/initializer and adapter metadata that Providers can fetch and verify independently. The catalog MAY retain graph-valid split candidates without Provider bindings; an adapter that exposes only one opaque atomic node remains runnable as one role and MUST NOT be falsely split.
- **FR-009**: After Selection, each Provider MUST fetch the required canonical components, assemble one local role, validate complete graph/initializer coverage, and execute that role with ONNX Runtime.
- **FR-010**: The deployed runtime MUST NOT require PyTorch or Transformers; those may exist only in an offline export path that produces canonical artifacts.
- **FR-011**: A role MUST become runnable when its Selection, local role preparation, and all sealed input dependencies are ready. The system MUST NOT introduce a second global preparation commit or an all-Provider readiness barrier.
- **FR-012**: The final response MUST satisfy a sealed result contract and represent the complete application result, not merely one local tensor, one stage output, or one generated token.

#### NDN Dataflow

- **FR-013**: Persistent model objects MUST use the repository namespace, while request-scoped activations and collective partials MUST use the producing Provider's namespace.
- **FR-014**: Every cross-Provider activation, tensor partial, and collective result MUST be transferred through planned NDN Interest/Data exchange. Hidden TCP/RPC push, shared files, or opaque cross-Provider NCCL paths MUST NOT satisfy this requirement.
- **FR-015**: Every intermediate object name MUST bind the requester, request, attempt, plan, group and epoch, operation and round, producer role or rank, tensor and microbatch identity, content digest, and segment identity.
- **FR-016**: Consumers MUST fetch and authenticate a manifest before accepting segments, compare full names and declared layout/size/digests, tolerate valid duplicate or out-of-order delivery, and reconstruct an object exactly once.
- **FR-017**: Pipeline, tensor, and hybrid execution MUST use one dependency-driven scheduler. Any merge that requires computation MUST be an explicit execution role rather than an implicit group side effect.

#### Security, Recovery, and Evidence

- **FR-018**: The runtime MUST preserve NDNSF permission, NAC-ABE attribute, UserToken, ProviderToken, signature, and replay controls throughout request, ACK, Selection, tensor Data, and response handling.
- **FR-019**: Protected execution MUST bind a Provider-specific grant and local group capability to the exact request, attempt, plan, policy snapshot, role or rank, protection epoch, and local wrapped key before ONNX Runtime receives plaintext.
- **FR-020**: Retry, replanning, cancellation, Provider restart, and permanent dependency failure MUST advance or fence the affected attempt, plan, generation, group epoch, and protected runtime. Old output may be adopted only when the new attempt explicitly proves identical authority and result contracts; otherwise stale work MUST NOT publish accepted output. Protected plaintext MUST be released and zeroized.
- **FR-021**: Canonical artifact, assembled role, loaded runtime, exact-forward, and KV-state reuse MUST require the appropriate exact identity and compatibility evidence and MUST NOT change accepted output semantics. Missing or partial compatibility MUST fall back to clean computation or a new plan.
- **FR-022**: Each run MUST emit bounded evidence for ACK closure, plan commit, Selection acceptance, role preparation, exact dependency fetch/publication, execution, cancellation/failure, and final response. Resource and cache evidence MUST be the minimum needed for placement, and evidence MUST exclude secrets, plaintext tensors, unauthorized cross-tenant state, and mutable runtime authority.

#### Existing Implementation Convergence

- **FR-029**: Before changing a production behavior, the implementation task MUST identify its current owner symbols, current regression coverage, and the smallest target gap. Conforming current code MUST be reused; partially conforming code MUST be repaired at its existing cohesive boundary; a duplicate runtime/planner/dataflow/security/deployment subsystem or a new production owner MUST NOT be introduced without an explicit code-aware gap, migration boundary, and regression justification in the approved plan.

#### Ordered Verification Gates

- **FR-023**: The unit gate MUST cover graph/split legality, one-to-one role ownership, immutable identities, deterministic names, manifest/segment validation, state transitions, replay, cancellation, protected-runtime rejection, and zeroization.
- **FR-024**: The integration gate MUST exercise the real production codecs and handlers through a four-Provider/four-role REQUEST-to-RESPONSE lifecycle and a deterministic packet matrix covering loss, repair, reorder, duplicate, replay, corruption, stale identity, cancellation, and missing peers.
- **FR-025**: The MiniNDN+CPU gate MUST run separate real processes and NFD routing for at least one pipeline, one two-rank/two-Provider tensor group, and one hybrid plan, using a small inspectable ONNX model and an unsplit oracle. Host NFD or an in-process-only result MUST NOT satisfy this gate.
- **FR-026**: TigerCluster submission MUST remain disabled until FR-023 through FR-025 pass on the same source state and the exact immutable deployment candidate is built inside its target ABI and passes local CPU qualification.
- **FR-027**: TigerCluster qualification MUST begin with a read-only environment preflight, verify the candidate digest before running, and advance through bounded network/reference, CPU/no-GPU, single-GPU, cross-Provider multi-GPU, and complete-answer stages. A failed stage MUST stop later stages and preserve its evidence.
- **FR-028**: Operator configuration MAY select policies, bounds, repositories, security material, observability, and runtime limits, but MUST NOT predeclare the request-specific Provider list, role assignment, stage map, or final split.

### Key Entities

- **Operation Request**: Signed application intent bound to one logical invocation and one or more recovery attempts.
- **Provider Offer**: Sanitized, signed, request-bound evidence of willingness, capabilities, resource envelope, residency, boot epoch, and runtime compatibility; it is not execution authority.
- **ACK Closure**: Immutable offer set and deadline record that linearizes planning.
- **Execution Role**: The smallest complete unit assigned to one Provider, including a pipeline stage or tensor rank.
- **Tensor Group**: Metadata and collective contract that coordinates multiple rank roles without becoming a role shared across Providers.
- **Placement Plan**: One immutable graph split, one-to-one role/Provider assignment, dependency graph, result contract, and security bindings.
- **Role Assembly and Dataflow Contract**: Provider-local authority describing what to fetch and assemble, what exact inputs to request, what outputs may be published, and when the role is ready.
- **Tensor Object Manifest**: Signed description of one request-scoped tensor object, its producer, layout, size, segments, and digests.
- **Protection Grant and Capability**: Provider-local authorization and key projection that permit only the selected role and dataflow for one protection epoch.
- **Verification Candidate**: Immutable deployment artifact and manifest promoted unchanged from local qualification to TigerCluster.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every target behavior in the design authority maps to one requirement or is explicitly listed as out of scope; no active requirement depends on Spec170 task numbering.
- **SC-002**: All relevant unit tests pass from one clean build, and every required rejection path demonstrates zero accepted output and zero leaked execution authority.
- **SC-003**: The four-Provider/four-role integration lifecycle and the complete deterministic packet-fault matrix pass in three independent process runs with no hang, duplicate execution, or partial response.
- **SC-004**: Pipeline, two-rank tensor, and hybrid MiniNDN+CPU cases each pass in three independent clean runs; every planned edge is observed with matching publish/fetch names and every final output exactly matches the unsplit oracle.
- **SC-005**: Loss, corruption, stale identity, replay, missing rank, and cancellation cases terminate within their declared bounds, identify the first failed event or object, and produce no accepted final response.
- **SC-006**: No TigerCluster job is submitted unless the recorded unit, integration, MiniNDN+CPU, and exact local-candidate gates all show PASS for the same source and candidate identities.
- **SC-007**: The candidate digest measured on TigerCluster exactly matches the locally qualified digest, and the cluster does not rebuild or replace its runtime libraries.
- **SC-008**: A single-GPU qualification produces a complete accepted result with the intended GPU execution provider and zero CPU fallback.
- **SC-009**: A cross-Provider GPU qualification assigns one role or rank per Provider, exchanges every cross-Provider tensor through the sealed NDN dataflow, and matches the frozen reference result.
- **SC-010**: Every failed gate preserves a concise manifest, first-failure diagnostic, relevant log, and source/candidate identity; superseded raw runs are not treated as current evidence.
- **SC-011**: Every implementation task names the existing owner and regression inputs it reuses, or records a reviewed reason that no suitable owner exists; the delivered source contains no parallel Spec174-only runtime path and all relevant previously passing regressions remain passing.

## Out of Scope

- Redesigning the generic NDNSF request/permission/token framework.
- Model training, online conversion, or a deployed PyTorch/Transformers runtime.
- Assigning multiple roles to one Provider in one attempt.
- Spreading one logical role across multiple Providers.
- Using `DEVICE_SET`, a LayerReuseFirst default, an implicit global readiness
  barrier, or an opaque cross-Provider collective transport.
- Proving every possible ONNX model family; local correctness uses one small,
  inspectable model, while cluster qualification uses a frozen representative
  large model.
- Performance or scalability claims before correctness gates pass. Warm-cache
  measurements, placement optimization, and large repeated campaigns are
  follow-on evaluation work, not completion criteria for the core design.

## Assumptions

- The referenced slide PDF remains unchanged during this feature. A changed
  hash requires an explicit reconciliation before implementation continues.
- Existing NDNSF-DI and Spec170 production code is the default implementation
  baseline. It MUST be reused unless a code-aware mapping shows that it
  conflicts with this specification; historical pass claims are revalidated
  by the new gates, but revalidation is not a reason to rewrite conforming code.
- The current dynamic NDNSF request, ACK, Selection, Response, permission, and
  token mechanisms remain available and are not replaced.
- Four or five distinct Providers may be represented by separate CPU processes
  locally even when only one physical host is available.
- The small ONNX fixture is deterministic and exposes inspectable pipeline,
  tensor, and hybrid cuts; exact oracle comparison is therefore meaningful.
- The immutable deployment candidate is built and fully qualified locally, then
  transferred to TigerCluster and verified by content digest.
- Cluster unavailability, allocation failure, or unavailable hardware is a
  truthful BLOCK result rather than permission to skip or weaken a lower gate.
