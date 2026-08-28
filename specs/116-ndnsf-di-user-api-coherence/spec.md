# Feature Specification: Coherent NDNSF-DI User API

**Feature Branch**: `116-ndnsf-di-user-api-coherence`

**Created**: 2026-07-16

**Status**: Complete

**Input**: Audit the current user-facing NDNSF-DI Python API, make deployment,
discovery, request, result, provider, and optimizer flows logically coherent and
easy to use, then provide an implementation plan and cohesive task list.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Request, Prepare on Demand, and Invoke (Priority: P1)

As an application developer, I can request inference against an Application-
signed deployment definition and obtain a typed result even when no Provider
has loaded the model yet. The request handle exposes preparation progress while
NDNSF-DI selects responsibilities, obtains artifacts, verifies them, loads and
warms the model, certifies readiness, and then executes the request.

**Why this priority**: The current public surface exposes the correct low-level
mechanisms but does not connect them into the workflow users actually perform.

**Independent Test**: Run the documented request-to-result quickstart with no
pre-existing deployment against a local deterministic adapter and a MiniNDN
multi-provider fixture. One `request` call creates a durable request, reports
per-role preparation progress, reaches certified readiness, and returns a typed
result.

**Acceptance Scenarios**:

1. **Given** a valid Application-signed definition and no ready Providers,
   **When** the application calls `request(definition, ...)`, **Then** it receives
   a durable request handle immediately; ACKs describe willingness and current
   availability, Selection assigns responsibilities, the responsibility/lease
   intent is certified, selected Providers are prepared, exact readiness
   evidence is validated, and only then is model inference executed.
2. **Given** selected Providers that must fetch or load artifacts, **When** the
   requester observes the handle, **Then** it can inspect aggregate and
   per-role progress through `deployment_status()` and `events()` and can
   recover authoritative status after lost or reordered progress events.
3. **Given** an already active compatible revision, **When** it is requested,
   **Then** the same ensure-deployment path revalidates exact readiness and may
   reuse it without restaging. Whether a selection policy prefers READY offers
   over NEEDS_PREPARATION offers is an optimization decision, not a correctness
   condition or a new request semantic.
4. **Given** an unsigned or unauthorized definition, invalid policy result,
   stale revision, forged readiness receipt, or failed provider preparation,
   **When** the request proceeds, **Then** the operation fails
   closed without publishing an active revision or silently falling back to an
   unsafe path.

---

### User Story 2 - Discover and Invoke Active or On-Demand Targets (Priority: P1)

As a remote requester, I can discover Application-authorized ACTIVE or
ON_DEMAND deployments by service, inspect typed summaries, and request inference without
using a raw `ServiceUser`, parsing dictionaries, or knowing deployment-control
wire details.

**Why this priority**: A distributed deployment is useful only if requesters
other than its creator can find and safely invoke it.

**Independent Test**: A second client process discovers an Application-signed
on-demand definition in MiniNDN, requests it before any Provider is ready,
observes preparation, receives a result, and recovers the same durable request
after process restart. The same test also covers an already ACTIVE deployment.

**Acceptance Scenarios**:

1. **Given** published signed definitions and active revisions, **When** the
   requester discovers by service and constraints, **Then** it receives typed
   summaries identifying ON_DEMAND definitions or ACTIVE immutable revisions
   and no untyped wire dictionaries.
2. **Given** an ON_DEMAND definition reference, **When** it is passed to
   `request`, **Then** the requester cannot alter Application intent and the
   request resolves and fences one immutable revision before Provider work.
3. **Given** the same valid signed `DeploymentDefinition` value is available
   directly, **When** either `InferenceApplication.request` or
   `InferenceClient.request` receives it, **Then** both use the identical
   distributed request implementation, validation, transport, state machine,
   status, security evidence, and result-handle contract.
4. **Given** an ACTIVE deployment reference, **When** it is passed to `request`,
   **Then** the request remains fenced to that revision even if a new revision
   is activated concurrently.
5. **Given** a serialized deployment or request reference, **When** a client
   restarts, **Then** it can rebind a handle and continue status/result access
   without treating the reference as new authority.

---

### User Story 3 - Extend Optimization and Provider Behavior Safely (Priority: P1)

As an optimization or provider developer, I can replace selected policies and
register model runners through explicit, typed, documented extension points,
without depending on accidental package exports, raw metadata-key conventions,
duplicate facade implementations, or application-only lifecycle internals.

**Why this priority**: A separate optimization team must be able to use NDNSF-DI
without learning private implementation structure or weakening trust boundaries.

**Independent Test**: An external package replaces one policy in the default
suite and registers one runner using only the documented SDK; import-contract,
type-check, deterministic decision, and provider-startup tests pass.

**Acceptance Scenarios**:

1. **Given** the default optimization suite, **When** one typed policy is
   replaced, **Then** all omitted policies use versioned defaults and suite
   identity is derived deterministically from public descriptors.
2. **Given** a provider implementation, **When** it calls the preferred serving
   API, **Then** runner execution is available while lifecycle receipt issuance
   remains confined to its explicit administrative port.
3. **Given** any canonical public package, **When** its export manifest is
   inspected, **Then** only approved public names appear; imported modules,
   typing helpers, dataclass helpers, and internal facades do not leak.

---

### User Story 4 - Migrate Existing Applications Predictably (Priority: P2)

As a maintainer of an existing NDNSF-DI application, I receive actionable
deprecation guidance and a bounded migration path from current root exports and
ambiguous convenience calls, while security and durable-state behavior remain
unchanged during the compatibility window.

**Why this priority**: The root compatibility manifest is intentional Spec 111
work and must not be removed abruptly merely to make the preferred API smaller.

**Independent Test**: Compatibility tests execute maintained old examples,
assert one warning per deprecated surface, and compare their normalized result
and security evidence with the canonical path.

**Acceptance Scenarios**:

1. **Given** an old root import or convenience alias, **When** it is used during
   the compatibility window, **Then** it delegates to one canonical owner and
   reports its exact replacement.
2. **Given** the old overloaded local `submit` form, **When** it is migrated,
   **Then** local/test execution uses an explicitly named adapter rather than a
   runtime branch inside the network request API.
3. **Given** migration telemetry and release criteria, **When** compatibility
   removal is considered, **Then** removal requires a separately approved
   change and cannot happen solely because Spec 116 implementation is complete.

### Edge Cases

- An application restarts while an on-demand request is fetching, verifying,
  loading, warming, preparing, or committing selected Providers.
- An ACK says READY but its evidence is stale, bound to another boot epoch,
  artifact digest, adapter, role, definition, or revision.
- Progress events are duplicated, reordered, lost, or arrive after a terminal
  failure; an authoritative status query must still converge.
- A Provider is still fetching assignment keys/artifacts before its
  `CollaborationContext` exists, or a forged/stale status Data packet attempts
  to advance the requester's view.
- One role becomes READY while another fails, the request is cancelled during
  preparation, or the overall deadline expires before execution begins.
- A deployment revision changes between discovery and request publication, or an
  untrusted NDNSD hint points at a forged activation record.
- `timeout` and absolute `deadline` are both supplied, are timezone-naive, or
  are already expired.
- Deployment discovery returns malformed, unsigned, expired, draining,
  duplicate, or unauthorized ON_DEMAND/ACTIVE advertisements.
- An ON_DEMAND definition's bound coordinator is unavailable or a discovery
  hint substitutes a different coordinator identity.
- A custom optimization suite omits most policies, returns a candidate outside
  its bounded input set, or changes state while a decision graph is running.
- A Python optimizer raises, times out, or returns an object that violates the
  typed result contract.
- A provider serves inference correctly but lacks authority to emit deployment
  readiness or lifecycle receipts.
- A policy ranks NEEDS_PREPARATION ahead of READY; correctness must still hold
  by completing preparation rather than assuming the chosen Provider is ready.
- A legacy call relies on numeric deadline heuristics or dynamically delegated
  private methods.
- Streaming lifecycle events are confused with streaming model output.
- Optional ONNX, CUDA, Qwen, or NDN dependencies are absent at import time.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The preferred Python API MUST expose one documented composition
  root for application-created deployment and invocation, plus explicit
  requester-only and provider roles; it MUST NOT create a second execution or
  lifecycle authority.
- **FR-002**: The composition root MUST orchestrate the existing canonical
  deployment, client, engine, provider, journal, fencing, and receipt owners.
  `InferenceApplication.request` MUST be a strict delegate to the same
  `InferenceClient.request` implementation used remotely, not a distinct
  creator request path.
- **FR-003**: Only the configured Application role MAY create and sign a
  `DeploymentDefinition`. Canonical `request(definition, ...)` MUST verify
  creator identity and authorization and MUST route ON_DEMAND realization to
  the authorized deployment coordinator bound by that definition. That
  coordinator, not a remote requester, MUST perform bounded policy resolution,
  responsibility assignment, Provider preparation, readiness certification,
  and inference within one durable request.
- **FR-004**: Advanced callers MUST retain explicit low-level deployment phase
  APIs. Optional `deploy(definition)` MUST remain available for prewarming and
  operations, return a bound durable deployment handle, and reuse exactly the
  same ensure-deployment coordinator and status contracts as request-triggered
  deployment; neither path may implement a second lifecycle state machine.
- **FR-005**: `DeploymentDefinition` MUST provide typed fields for Application
  creator identity, authorized deployment-owner identity, deployment/service
  identity, exact or authorized model alternatives, immutable artifacts,
  request semantics, objectives, hard constraints, and an allowlisted
  optimization profile. Concrete partition
  graph, roles, Provider assignment, execution target, and mutable runtime
  state MUST be policy-resolved revision/decision outputs rather than
  Application-authored definition facts; a generic configuration map MUST NOT
  be the sole contract for controlling semantics.
- **FR-006**: Deployment definitions MUST NOT serialize Python policy objects,
  arbitrary executable code, credentials, envelope keys, or lifecycle receipts.
- **FR-007**: A deployment handle MUST bind one deployment intent/revision and
  expose `status`, `wait_until_active`, `refresh`, and an always-serializable
  `DeploymentHandleRef`. Its invocable `DeploymentRef` MUST become available
  only after authenticated ACTIVE evidence exists; rebinding either reference
  MUST NOT grant additional authority.
- **FR-008**: A typed deployment catalog MUST expose `discover` and `get`
  without requiring callers to pass raw `ServiceUser` objects or consume
  dictionaries. It MUST distinguish an Application-signed
  `DeploymentDefinitionRef` in ON_DEMAND state from an authenticated ACTIVE
  `DeploymentRef`. NDNSD metadata MAY bootstrap discovery only as an untrusted
  locator/digest hint and MUST NOT establish either authority or readiness. The
  definition reference MUST bind the authorized deployment coordinator service;
  a requester MUST NOT need or be allowed to execute the Application's policy
  profile locally.
- **FR-009**: Canonical discovery MUST fetch a versioned, digest-bound
  Application-signed definition record for ON_DEMAND deployments. For ACTIVE
  deployments it MUST additionally fetch and validate the
  `DeploymentActivationRecord`, authorized deployment-owner signature,
  activation certificate, freshness/expiry, definition/revision digests, and
  revocation/rollover fence. These APP-level records MUST NOT add a second
  NDNSF invocation wire protocol.
- **FR-010**: The canonical `request` MUST have one network inference meaning,
  accept an Application-owned `DeploymentDefinition`, signed
  `DeploymentDefinitionRef`, ACTIVE deployment handle/reference, or normalized
  typed `RequestableDeployment` through both `InferenceApplication` and
  `InferenceClient`, return
  the same handle contract, and MUST NOT
  branch between local and network behavior using empty-string sentinels.
- **FR-011**: Canonical request timing MUST accept an aware absolute `deadline`
  or a duration `timeout`, reject ambiguous combinations, and MUST NOT infer
  units or absolute/relative meaning from numeric magnitude.
- **FR-012**: `request` MUST return a durable bound `InferenceRequestHandle`
  exposing `status`, `wait`, `result`, `result_async`, `cancel`, and lifecycle
  `events`; synchronous and asynchronous result operations MUST return the same
  documented `InferenceResult`, and their local wait budget MUST be named
  `wait_timeout` so it cannot be confused with the request deadline.
- **FR-013**: Lifecycle event streaming MUST be named `events`; model token or
  tensor streaming MUST use a distinct output-stream contract.
- **FR-014**: A synchronous convenience operation MAY exist only as a strict
  composition of `request(...).result(...)`, with no alternate execution path.
- **FR-015**: Local deterministic/test execution MUST use an explicitly named
  local adapter or harness rather than an overloaded production `request` form.
  Existing overloaded `submit` remains compatibility-only and MUST delegate to
  the canonical network `request` or the explicit local harness as applicable.
- **FR-016**: A normal NDNSF ACK MUST mean willingness/capability to accept a
  request, not completed deployment. Its typed NDNSF-DI payload MUST declare
  `READY`, `NEEDS_PREPARATION`, or `UNAVAILABLE` for the candidate role and
  bind any READY claim to exact revision/artifact/adapter/boot-epoch evidence.
  ACK readiness is advisory and MUST be revalidated after Selection.
- **FR-017**: Selection MUST assign selected Provider responsibilities and bind
  the deployment intent, role, request, attempt, lease, and fence. It MUST NOT
  by itself assert that the selected Provider is ready or authorize inference
  execution.
- **FR-018**: Selection MUST carry the existing request execution-intent
  certificate that commits exact responsibility, membership, lease, and fence;
  this certificate MUST be validated before Provider preparation but MUST NOT
  be interpreted as model readiness. One shared ensure-deployment coordinator
  MUST then drive each selected role through bounded, idempotent
  preparation states including ACCEPTED, FETCHING, VERIFYING, LOADING, WARMING,
  and READY, with FAILED, CANCELLED, and EXPIRED terminal outcomes. It MUST
  validate complete signed `ProviderReadiness` evidence and a readiness barrier
  for all required roles before invoking any model runner.
- **FR-019**: NDNSF Collaboration MUST expose one application-neutral operation
  status capability over the existing selection-status path. A Provider MUST
  be able to report `ServiceOperationStatus` before a `CollaborationContext`
  exists and through `CollaborationContext` after handler dispatch; a requester
  MUST be able to query the latest signed per-member snapshot and perform
  bounded watch/wait through the Python binding. The snapshot MUST bind request,
  selection digest, Provider, role/operation ID, attempt/epoch, monotonic
  sequence, freshness, generic state/progress/reason, and optional bounded
  application details. Signature/binding/freshness validation and stale/replay
  rejection are mandatory. Request-triggered preparation MUST project these
  generic snapshots into aggregate and per-role `DeploymentStatus` on the
  request handle, including single-role requests and recovery after event loss.
  Collaboration handler registration and `RequestCollaboration` MUST wire this
  status path by default; users MUST NOT need an undocumented combination of
  `setSelectionStatusQueryable` and tracked-request flags.
- **FR-020**: No new generic NDNSF Request/ACK/Selection/Response message kind
  or parallel status protocol is permitted for deployment progress. The
  existing `SELECTION-STATUS` query/reply, `SelectionExecutionStatus`,
  `ServiceOperationStatus`, signed NDN Data, and optional collaboration event
  publication MUST be extended compatibly and reused. Generic NDNSF states
  describe operation lifecycle only; NDNSF-DI continues to own model/artifact,
  adapter/GPU, FETCHING/VERIFYING/LOADING/WARMING/READY, exact
  `ProviderReadiness`, and the all-role readiness barrier. Final Response
  remains the inference result, never a deployment-progress message.
- **FR-021**: The request deadline MUST bound preparation plus execution.
  Cancellation, expiry, requester restart, partial-role failure, and network
  partition during preparation MUST use existing abort, fencing, journal, and
  orphan-recovery rules and MUST NOT leak reservations or publish partial
  readiness as ACTIVE.
- **FR-022**: Provider-selection policy MAY prefer exact READY offers to reduce
  deployment work, but this ranking is an optimization seam. Core correctness
  MUST accept any valid selected candidate and either establish certified
  readiness or fail closed; no mandatory algorithm or performance claim is
  introduced by this feature.
- **FR-023**: The provider's preferred application API MUST have one serving
  verb. Deployment stage/activate/drain/delete and signed receipt emission MUST
  remain on a separately constructed advanced `ProviderAdminPort` with existing
  authorization checks; obtaining a serving facade or registering a runner MUST
  NOT expose or confer administrative authority.
- **FR-024**: Optimization policies and `RunnerAdapter` MUST remain independent
  extension families; Runner selection MUST NOT be merged into placement or
  provider-selection policy.
- **FR-025**: The optimization SDK MUST support replacing any subset of policies
  over versioned defaults and MUST derive suite identity from stable public
  descriptors/configuration rather than requiring an arbitrary caller-supplied
  digest.
- **FR-026**: Each of the ten policy seams MUST expose a dedicated typed
  request/result contract with required fields, units, candidates, upstream
  evidence, and outputs declared explicitly; aliases or typed accessors over one
  generic metadata map are insufficient as the long-term public interface.
- **FR-027**: Policy execution MUST validate bounded candidates, decision
  evidence, budget, epoch, and deterministic suite identity before mutation or
  provider action.
- **FR-028**: Canonical packages MUST use explicit allowlisted exports; imported
  modules, typing symbols, dataclass helpers, implementation facades, and
  compatibility-only names MUST not appear in those manifests.
- **FR-029**: Each public facade name MUST have one behavior owner in its
  canonical APP role module. The preferred `api` namespace MUST be an explicit
  re-export-only package, not another behavior implementation; same-named
  classes in `app_sdk.facades` MUST become private collaborators or bounded
  compatibility adapters, and dynamic `__getattr__` delegation MUST NOT define
  the canonical API contract.
- **FR-030**: Existing root exports and convenience aliases MUST follow a
  documented compatibility window with precise replacement warnings,
  observability, exit criteria, and an explicitly approved removal step.
- **FR-031**: The English and Chinese README, API reference, examples, type
  information, and runtime signatures MUST agree and their primary snippets
  MUST execute as contract tests.
- **FR-032**: Production configuration MUST keep durable `state_root`, request
  envelope key, signature, fencing, replay, receipt, and fail-closed requirements
  explicit; convenience defaults MUST NOT weaken them.
- **FR-033**: Final network/security acceptance MUST bind one candidate identity
  to a complementary campaign: a deterministic local security/failure matrix,
  an exact-name signed-catalog MiniNDN fixture, and a multi-Provider readiness/
  execution MiniNDN fixture. Across the campaign it MUST cover creator, remote
  requester, discovery, revision rollover, on-demand cold preparation,
  already-ready reuse, progress loss/recovery, restart recovery, invalid
  evidence, and cancellation. A single monolithic topology script is not
  required; Docker/iTiger/GPU benchmarking is outside this API-coherence
  feature.
- **FR-034**: Implementation MUST preserve Spec 111 Core/APP dependency
  direction, distributed Prepare/Commit/Abort consistency, request fencing,
  orphan recovery, and the separation between policy decisions and execution.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: After definition construction, the primary creator workflow is
  `request` then `result`; it requires no explicit deploy/wait, receipt,
  revision, role, or Provider plumbing and exposes preparation through the same
  request handle.
- **SC-002**: One documented requester workflow discovers and invokes ACTIVE
  and ON_DEMAND deployments without raw `ServiceUser`, untyped dictionaries, or
  application-supplied wire names, and rejects every invalid definition or
  activation-record cell.
- **SC-003**: Canonical `request` has one semantic form, zero underscore-prefixed
  public parameters, zero empty-string mode sentinels, and zero numeric deadline
  heuristics.
- **SC-004**: Canonical public export tests report zero accidental helper,
  module, internal-facade, or compatibility-only exports.
- **SC-005**: Exactly one implementation owner exists for each public facade;
  the preferred namespace contains re-exports only, and canonical API tests
  require zero dynamic `__getattr__` behavior.
- **SC-006**: The external optimizer fixture replaces one policy while relying
  on defaults for the remainder, uses no private imports, and passes type,
  determinism, candidate-bound, and evidence contract tests.
- **SC-007**: English and Chinese primary examples execute unchanged as tests
  against the packaged canonical API in CPU-only and optional-dependency-absent
  environments.
- **SC-008**: The candidate-bound local and complementary MiniNDN
  deploy/discover/request/recover campaign passes 100% of its declared
  correctness and security cells, including pre-context/context status,
  multi-role query/watch/wait, event-loss recovery, forged/stale/replay
  rejection, and proof that generic DONE cannot bypass exact DI readiness;
  measured failures remain reported rather than retried away.
- **SC-009**: Compatibility tests preserve normalized behavior and security
  evidence for all maintained aliases and report one actionable replacement
  warning per deprecated surface.
- **SC-010**: Strict Spec Kit structure, traceability, task-cohesion, and
  code-aware semantic audit report zero unresolved Critical or High findings
  before implementation begins.

## Assumptions

- Python is the primary external application and optimizer interface. The only
  planned C++/wire evolution is the audited additive generic member snapshot on
  existing `SELECTION-STATUS`; base messages and NFD/SVS/NAC-ABE remain unchanged.
- The current Spec 111 implementation is the baseline and its security and
  distributed-consistency contracts remain controlling.
- `DeploymentDefinition` is Application-authored intent and may be requested
  directly by its creator or referenced by an authorized remote requester.
  Optimization policies
  resolve concrete model/partition/placement/execution-target decisions into the immutable
  revision and later request certificate; deployment operators execute that
  revision but cannot author or mutate the definition.
- `APPClient`, `APPDeployment`, and `APPProvider` remain usable advanced role
  components; the preferred composition root delegates to them rather than
  replacing their authorities.
- The root compatibility manifest remains during this feature and removal is a
  later, explicitly approved migration step.
- MiniNDN is the authoritative network validation environment for this feature.
- Explicit predeployment is an optional latency/operations tool, not a
  prerequisite for request correctness. READY-first selection is an optimizer
  choice and is not required for baseline acceptance.
- `InferenceApplication` and `InferenceClient` differ in authority-bearing
  creation/administration methods, not in distributed request behavior.

## Out of Scope

- New model partitioning, placement, scheduling, caching, recovery, or backend
  optimization algorithms.
- New generic NDNSF message kinds or changes to NFD, SVS, or NAC-ABE. A bounded,
  backward-compatible NDNSF C++/Python Collaboration status API extension over
  existing `SELECTION-STATUS` and signed Data is in scope; NDNSF-DI model
  preparation semantics carried as bounded application details remain APP-owned.
- Docker/Apptainer image construction, iTiger deployment, GPU throughput, or
  Qwen scaling experiments.
- A graphical user interface, remote policy-code upload, or arbitrary plugin
  execution from deployment definitions.
