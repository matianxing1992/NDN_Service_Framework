# Implementation Plan: Coherent NDNSF-DI User API

**Branch**: `116-ndnsf-di-user-api-coherence` | **Date**: 2026-07-16 |
**Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/116-ndnsf-di-user-api-coherence/spec.md`

## Summary

Refine the Python-facing NDNSF-DI surface around one coherent request-to-result
journey. Canonical `request` accepts Application-signed deployment intent or an
ACTIVE revision, automatically ensures selected Providers are prepared, and
exposes deployment progress through the durable request handle before executing
inference. Extend the existing NDNSF `SELECTION-STATUS` path with a generic,
validated per-member Collaboration operation snapshot so pre-handler and
in-handler work are queryable without moving model/GPU semantics into Core.
Optional explicit `deploy` remains for prewarming but calls the same
ensure-deployment coordinator and status model. Add a thin
`InferenceApplication` composition root, a requester-only `InferenceClient`,
typed ACTIVE/ON_DEMAND discovery, and bound durable deployment/request handles.
The Application alone authors and signs deployment intent; policies resolve
concrete graph/role/Provider/execution-target decisions, and the deployment owner only
executes the immutable revision. Preserve existing Spec 111 owners, fail-closed
receipts, fencing, journals, distributed Prepare/Commit/Abort, policy seams,
and independent RunnerAdapter. Replace
accidental exports, overloaded request semantics, weakly typed optimizer
metadata, and contradictory documentation through bounded compatibility
adapters rather than immediate removal.

## Technical Context

**Language/Version**: Python 3.10+ public SDK; bounded C++17 NDNSF Collaboration
status extension and Python binding

**Primary Dependencies**: NDNSF Python binding, ndn-cxx/NFD, ndn-svs,
NAC-ABE, existing NDNSF-DI Core/APP modules; ONNX/CUDA/Qwen optional

**Storage**: Existing request/deployment journals under explicit production
`state_root`; keys remain external files/providers

**Testing**: pytest contract/unit/import/type checks, example extraction tests,
local deterministic adapters, MiniNDN multi-process integration

**Target Platform**: Linux application, provider, and MiniNDN environments;
CPU-only import and contract validation is mandatory

**Project Type**: Existing Python package over C++/NDNSF distributed runtime

**Performance Goals**: Already-ready deployments add no deployment work beyond
readiness revalidation; cold deployments perform only the bounded preparation
rounds required for correctness. READY-first ranking remains an optimizer
choice and no latency improvement is an acceptance claim.

**Constraints**: Preserve security, fencing, durable recovery, dependency
direction, and compatibility window; no arbitrary policy-code serialization

**Scale/Scope**: Python public API, one backward-compatible generic NDNSF
Collaboration status substrate, optimizer SDK contracts, provider surface,
documentation/examples, and local/MiniNDN acceptance only

## Constitution Check

### Before implementation

- **Canonical Dynamic Runtime**: PASS. The plan changes Python composition and
  additively extends existing selection-status payload/API; it does not alter
  unified NDNSF names, base messages, or Targeted invocation semantics.
- **Security In Data Path**: PASS. Receipt, token, NAC-ABE, replay, key, and
  fencing checks remain mandatory and gain negative acceptance tests.
- **CodeGraph First**: PASS. Current public owners, callers, exports, and docs
  were traced with CodeGraph before design.
- **Spec-Driven Change**: PASS. Spec 116 owns this multi-file API evolution.
- **Right Validation Scope**: PASS. Local contract tests plus MiniNDN network
  and security acceptance; no host-NFD final claim.
- **Cohesive Tasks**: PASS. Tasks combine contract, implementation, tests, and
  evidence for one behavior; no mechanical test/code/evidence chains.

### Post-design gate

No constitutional violation is accepted. Dynamic compatibility is bounded but
not expanded. Only the audited additive `SELECTION-STATUS` member snapshot and
its C++/Python APIs are permitted; any base message, NFD/SVS/NAC-ABE, or
non-additive wire change discovered during implementation requires a new audit
finding and explicit scope decision.

## Architecture and Dependency Direction

```text
Application code
  |-- InferenceApplication ----------- definition authority + composition
  |      |-- ApplicationDefinitionSigner
  |      |-- InferenceClient.request - same distributed request implementation
  |      |      `-- APPClient -------- durable request authority (existing)
  |      |      `-- EnsureDeploymentCoordinator
  |      |              |-- APPDeployment / lease owner (existing)
  |      |              `-- readiness/status validation (existing primitives)
  |      `-- DistributedInferenceEngine / OptimizationSuite
  |
  |-- InferenceClient ---------------- ACTIVE/ON_DEMAND discovery + request
  |      |-- DeploymentCatalog
  |      `-- APPClient -> definition-bound authorized remote coordinator
  |
  `-- InferenceProvider -------------- serve model/runner only
         `-- RunnerAdapterRegistry

Advanced coordinator construction
  |-- ProviderAdminPort -------------- authenticated lifecycle actions
  `-- InferenceApplication.deploy ---- optional prewarm; same coordinator

Bound views, not authorities:
  DeploymentHandle -> DeploymentHandleRef + ACTIVE-only DeploymentRef + owner
  InferenceRequestHandle -> RequestRef + request journal owner

NDNSF Collaboration status substrate
  |-- existing SELECTION-STATUS query/reply + signed Data validation
  |-- per-member ServiceOperationStatus snapshot + monotonic sequence
  |-- Provider pre-context reporter + CollaborationContext reporter
  `-- requester query/watch/wait; no model/GPU semantics

Deep Core / NDNSF runtime / NFD / SVS
```

The new facade never implements another journal, fabricates receipts, chooses
an unfenced revision, or calls provider processes directly. Application-owned
definition signing is the only new authority surface: it authenticates intent
that Spec 111 already requires but the current five-field dataclass does not
carry. `APPDeployment` verifies that signature and authorized creator before
side effects. It cannot create or mutate definition semantics. The shared
`EnsureDeploymentCoordinator` is an orchestration boundary, not a new authority
or second state machine: it drives existing decision, lease, stage, readiness,
commit, abort, journal, and recovery owners for both request-time preparation and
optional prewarming.

Both public roles accept the same `RequestableDeployment` and invoke the same
distributed `InferenceClient.request` implementation. `InferenceApplication`
adds definition signing and optional deployment administration; it adds no
creator-only request transport, state machine, or result type.

An ON_DEMAND `DeploymentDefinitionRef` includes the Application-authorized
deployment coordinator identity/service. A remote `InferenceClient` sends the
request through that coordinator; it does not instantiate the definition's OptimizationSuite,
execute Application policy code, or gain lifecycle authority. The authorized
coordinator owns the shared ensure operation and writes the authoritative
journal. An unavailable coordinator is a bounded request failure, not a reason
to let the requester reinterpret intent.

## Canonical Request and Preparation Protocol

```text
request(Application-signed definition/ref or ACTIVE ref)
  -> durable request CREATED / PLANNING
  -> resolve/freeze immutable model/graph/role plan revision and candidates
  -> normal NDNSF request and typed ACK deployment offers
       READY | NEEDS_PREPARATION | UNAVAILABLE
  -> provider-assignment policy and NDNSF Selection bind Providers to roles
  -> validate responsibility/membership/lease execution certificate
  -> PREPARING: ensure each selected role
       ACCEPTED -> FETCHING -> VERIFYING -> LOADING -> WARMING -> READY
  -> validate signed ProviderReadiness for exact
       request/attempt/revision/role/artifacts/adapter/boot epoch
  -> all-role readiness barrier
  -> execute (EXECUTING)
  -> final NDNSF Response contains inference result (COMPLETED)
```

ACK means willingness and current availability only. Selection assigns work but
does not prove readiness. NDNSF extends its existing signed `SELECTION-STATUS`
reply with a backward-compatible tuple of generic `ServiceOperationStatus`
member snapshots. Providers can update it before `CollaborationContext` exists
and through the context afterward; requester bindings expose query/watch/wait.
Collaboration publication may notify observers, but the validated latest
snapshot is authoritative after loss/reorder/restart. NDNSF-DI maps its
FETCHING/VERIFYING/LOADING/WARMING/READY details into that envelope and retains
exact readiness/barrier authority. No new base message kind or second status
protocol is introduced.

Explicit prewarming is merely:

```text
deploy(definition) -> EnsureDeploymentCoordinator.ensure(definition)
request(active handle/ref) -> same ensure operation revalidates/reuses readiness
```

It is never a prerequisite for request and cannot drift into a separate
deployment protocol.

The canonical behavior owners are fixed before implementation:

| Public role | Canonical implementation owner | Preferred namespace behavior |
|---|---|---|
| `InferenceApplication` | `app_sdk/application.py` | `api/__init__.py` re-export only |
| `InferenceClient` | `app_sdk/client.py` | re-export only |
| `InferenceProvider` | `app_sdk/provider.py` | re-export only |
| deployment/catalog/handle values | `app_sdk/deployment.py`, `app_sdk/catalog.py`, `app_sdk/contracts.py` | re-export only |
| request handle/result values | `app_sdk/client.py`, `app_sdk/contracts.py` | re-export only |
| optimizer SPIs | `sdk/contracts.py`, `sdk/suite.py` | no APP behavior |

`app_sdk.facades` may retain private network collaborators during migration but
MUST NOT own same-named public classes. The preferred `api` package contains no
state machine, journal, network operation, `__getattr__`, or subclass wrapper.

## Project Structure

### Documentation for this feature

```text
specs/116-ndnsf-di-user-api-coherence/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── traceability.md
├── tasks.md
├── audit-report.md
├── checklists/requirements.md
└── contracts/
    ├── collaboration-operation-status.md
    ├── public-python-api.md
    ├── deployment-request-handles.md
    ├── optimizer-provider-surface.md
    └── compatibility-migration.md
```

### Anticipated source changes

```text
NDNSF-DistributedInference/ndnsf_distributed_inference/
├── app_sdk/                 # sole role behavior owners and explicit exports
├── api/                     # preferred explicit re-exports only
├── sdk/                     # typed optimizer contracts and suite builder
├── planner/                 # versioned defaults; explicit exports
├── compatibility/           # bounded adapters and manifest
└── deployment.py            # legacy discovery delegates only

tests/python/
├── test_ndnsf_collaboration_operation_status.py
├── test_ndnsf_di_public_api_contract.py
├── test_ndnsf_di_user_journey.py
├── test_ndnsf_di_deployment_catalog.py
├── test_ndnsf_di_optimizer_public_sdk.py
└── test_ndnsf_di_api_compatibility.py

ndn-service-framework/{common.hpp,ServiceProvider.hpp,ServiceProvider.cpp,
                       ServiceUser.hpp,ServiceUser.cpp}
pythonWrapper/{src/ndnsf/_ndnsf.cpp,ndnsf/service.py,ndnsf/runtime_telemetry.py}
tests/                         # generic Collaboration status contract/regression
└── unit-tests/generic-dynamic-api-collaboration-status.t.cpp
```

These ownership locations are part of the contract. A file may be split only
if the audit manifest is updated first and still names exactly one behavior
owner; implementation MUST NOT create a parallel facade package.

## Implementation Strategy

### Phase A - Freeze the contract before moving behavior

Create executable import/signature/example tests and an approved export
manifest. These tests describe the desired workflow and expose current
contradictions without deleting compatibility names.

### Phase B - Compose existing owners

Implement Application-authored signed definitions, bound references/handles,
typed ACTIVE/ON_DEMAND catalog, and thin application/requester facades. Make
request the primary coordinator entry: the policy graph resolves concrete
variant/partition/role/assignment/execution-target facts, ACKs report deployment offers,
Selection assigns responsibility, and one shared ensure-deployment operation
establishes certified readiness before execution. Optional `deploy` invokes the
same operation without an inference input. Route every lifecycle/request
operation through existing owners. Add no alternate persistence or execution
path.

### Phase C - Clarify extension roles

Make optimizer request/result contracts genuinely typed, derive suite identity,
and expose partial replacement ergonomically. Separate provider serving from
administrative lifecycle actions while preserving RunnerAdapter independence.

### Phase D - Migrate and validate

Implement explicit compatibility delegates and warnings, update English and
Chinese docs/examples together, then execute local and MiniNDN user journeys,
restart, revision-race, malformed-discovery, invalid-receipt, and cancellation
cells.

## Security and Failure Model

- Definitions are signed by the configured Application identity; the
  deployment owner verifies creator authorization and cannot rewrite intent.
- References contain identity, not capability escalation; rebinding rechecks
  local configuration and remote evidence.
- ACK availability is advisory. A READY claim is accepted only after exact
  signed readiness revalidation for revision, role, artifact, adapter, and boot
  epoch; Selection alone never authorizes execution.
- Deployment activation and request execution remain receipt- and fence-gated.
- NDNSD discovery fields are untrusted locator/digest hints only. Canonical
  discovery fetches the referenced versioned `DeploymentActivationRecord` from
  existing signed NDN Data and rejects unsigned, stale, malformed, draining,
  revoked, or inconsistent records before returning invocable references.
- Request against an ACTIVE deployment freezes its immutable deployment revision and
  authenticated activation record before publication. Request against an
  ON_DEMAND definition resolves and freezes one immutable plan revision during
  PLANNING. In both cases the existing request Prepare/Commit path binds the
  request-specific responsibility/membership/lease certificate before Provider
  preparation. That certificate does not assert readiness; exact readiness and
  an all-role barrier separately gate model-runner invocation.
- The request deadline covers preparation and execution. Cancellation, expiry,
  requester crash, partial-role failure, or partition triggers fenced abort and
  orphan cleanup; partial readiness never becomes ACTIVE.
- Progress events are non-authoritative observations. Existing
  `SELECTION-STATUS` replies are Provider-signed and MUST be validated against
  selected Provider identity, request, selection digest, role/operation,
  attempt/epoch, sequence, and freshness before updating a handle. Monotonic
  generic snapshots control observation recovery; DI-owned exact readiness
  receipts control the model-execution barrier.
- Policy results remain candidate-bounded and evidence-bearing.
- Optimizer code is locally installed/configured, never received as deployment
  data.
- Timeouts and deadlines are explicit types; ambiguous values fail before
  network mutation.
- Compatibility delegates cannot bypass canonical validation.

## Validation Plan

1. Static/import gates: explicit exports, optional dependencies, signatures,
   type information, no public dynamic delegation.
2. Component gates: Application creator/signature authorization, policy-
   resolved revision identity, activation-record codec/trust validation,
   references, timing types, suite identity, catalog validation, handle
   recovery, and provider admin authorization.
3. Executable docs: English and Chinese primary snippets use the installed
   package and production security inputs.
4. Local journey: cold request, preparation progress, result, restart, rebind,
   rollover, cancel, optional prewarm/reuse using deterministic adapters.
5. Candidate-bound distributed campaign: run deterministic local negative and
   lifecycle cells, an exact-name signed-catalog MiniNDN fixture, and a
   multi-Provider readiness/execution MiniNDN fixture. Together they cover the
   controller/Application creator/deployment owner/remote requester boundary,
   ON_DEMAND and ACTIVE records, cold and already-ready preparation,
   dropped/reordered progress with snapshot recovery, pre-context progress,
   signed query/watch/wait, forged/stale/replayed rejection, exact readiness and
   revision fencing, provider failure, cancellation, and recovery. The fixtures
   remain separate so a transport failure cannot obscure a readiness failure.
6. Compatibility parity: maintained aliases delegate once, warn once, and
   produce normalized security/result evidence equivalent to canonical calls.

## Rollout and Rollback

1. Release the preferred namespace and explicit manifests while compatibility
   remains enabled.
2. Migrate repository examples and docs; collect compatibility usage evidence.
3. Keep old surfaces as delegates for the documented window.
4. If canonical flow regresses, revert the preferred facade/docs while leaving
   existing APP role components and compatibility owners intact.
5. Removal of compatibility surfaces is a later separately audited feature.

## Complexity Tracking

| Decision | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| One composition root plus requester/provider roles | Creator, requester, and provider have different authority, but creator needs one coherent workflow | One universal object would expose provider/admin authority to requesters |
| Bound handles plus serializable refs | Ergonomic status/result calls and durable process restart | Raw IDs repeat plumbing; serializing live handles would confuse identity with authority |
| Request-time ensure plus optional prewarm | The primary API must work before a model is loaded while operations still need reusable warm deployments | Requiring deploy-before-request exposes an optimization as mandatory workflow; hiding readiness would confuse willingness with ability to execute |
| Extend existing Collaboration status query | Long-running collaboration is generic, current `SELECTION-STATUS` is already queryable, and preparation begins before `CollaborationContext`; an additive member snapshot closes the reusable observability gap | APP-only events miss pre-context preparation; a new message kind or parallel Targeted status service duplicates the existing query path |
| Keep DI readiness above generic progress | Model/artifact/adapter/GPU readiness and the all-role barrier are inference semantics | Putting DI phases or readiness authority in NDNSF Core would reverse the Core/APP dependency direction |
| Typed optimizer contracts | Separate team needs stable semantics and validation | Generic metadata maps hide required inputs and make type checking ineffective |
| Compatibility window | Existing Spec 111 exports and examples are deployed | Immediate deletion would be an unsafe migration, not an API cleanup |
