# Tasks: Coherent NDNSF-DI User API

**Input**: Design documents from
`specs/116-ndnsf-di-user-api-coherence/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`,
`contracts/`, `quickstart.md`, `traceability.md`

**Tests**: Required. Each task combines contract-first tests, implementation,
focused validation, and evidence for one behavioral outcome. Test, code, and
evidence fragments are not separate tasks.

**Organization**: Ten cohesive tasks follow public ownership and end-to-end
behavior. The primary slice is cold request-to-result. Explicit predeployment is
validated only as reuse of the same ensure-deployment operation.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Safe to execute concurrently because the task has an independent
  source/evidence owner and does not mutate the same public contract.
- **[Story]**: Maps to a user story in `spec.md`.

## Phase 1: Freeze the Public and Protocol Contracts

**Purpose**: Make the revised request-time semantics executable before moving
behavior.

- [x] T001 [US1] Create the golden public-API, import, and protocol-contract suite in `tests/python/test_ndnsf_di_public_api_contract.py`, `tests/python/test_ndnsf_di_on_demand_protocol.py`, `tests/python/test_ndnsf_collaboration_operation_status.py`, and `tests/unit-tests/generic-dynamic-api-collaboration-status.t.cpp` plus an approved owner/export manifest under `tests/fixtures/ndnsf-di-user-api/`; encode one shared `RequestableDeployment` and identical `InferenceApplication.request`/`InferenceClient.request` signatures using parameter `deployment`, `DeploymentDefinitionRef`, the multi-role `ProviderDeploymentOffers` ACK envelope, the additive `SELECTION-STATUS` member snapshot with pre-context/context report and requester query/watch/wait, `DeploymentProgress`, `DeploymentStatus`, request-handle status access, optional `deploy`, timing, and the invariant that ACK means willingness, Selection/certificate means responsibility/membership/leases, generic progress is observation, signed all-role DI readiness means the model runner may execute, and Response means final inference result; retain expected failures for leaked exports, duplicate owners, public dynamic delegation, overloaded legacy `submit`, numeric deadline heuristics, unsigned definitions, detached raw discovery, forged/stale/replayed status, and README/signature disagreement (FR-001-FR-022, FR-028-FR-031; SC-001-SC-005, SC-007)

**Checkpoint**: Names, ownership, state transitions, wire reuse, and authority
boundaries fail executably where the current implementation is incomplete.

## Phase 2: Establish One Composition and One Preparation Operation

**Goal**: Build the shared correctness substrate used by both request-triggered
preparation and optional prewarming.

- [x] T002 [US1] Establish `ndnsf_distributed_inference.api` as explicit re-exports only and implement each behavior once in its fixed APP role owner: add `InferenceApplication` in `app_sdk/application.py`, consolidate the sole distributed request behavior in `InferenceClient.request`, make `InferenceApplication.request` a strict delegate with the identical `RequestableDeployment`/timing/options/handle contract, consolidate `InferenceProvider` in its role module, turn same-named `app_sdk.facades` classes into private collaborators or bounded compatibility adapters, remove canonical `__getattr__` dependence, preserve mandatory production `state_root` and exactly one envelope-key source, and close ownership/configuration/signature-parity/CPU-only import tests without introducing a new journal, lifecycle authority, or execution path (FR-001-FR-002, FR-010, FR-028-FR-029, FR-032, FR-034; SC-003-SC-005, SC-007)

- [x] T003 [US1] Implement one shared generic-status-plus-ensure contract in `ndn-service-framework/common.hpp`, `ServiceProvider.hpp/.cpp`, `ServiceUser.hpp/.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, `pythonWrapper/ndnsf/runtime_telemetry.py`, and the existing NDNSF-DI APP owners: add a backward-compatible indexed/canonical bounded member-status tuple to `SelectionExecutionStatus`/`SELECTION-STATUS`; extend `ServiceOperationStatus` with role/member, attempt/epoch, sequence, explicit progress-known, and bounded details; make collaboration registration queryable and `RequestCollaboration` track selected status bindings by default; validate Provider-signed Data and exact request/selection/Provider/role/freshness binding; thread the binding through pre-handler assignment preparation; expose `ServiceProvider` pre-context reporting, `CollaborationContext` reporting, and requester/Python query-watch-wait over the same latest snapshot with bounded retention and legacy empty-snapshot compatibility; then add typed/canonical NDNSF-DI codecs and validation for multi-role `ProviderDeploymentOffers`, the DI projection `DeploymentProgress`, aggregate `DeploymentStatus`, and exact `ProviderReadiness`; carry offers in the existing ACK payload; bind request/attempt/definition/plan-revision/role/Provider/lease/fence in Selection and its existing execution certificate; validate that responsibility/membership/lease certificate before preparation while explicitly not treating it as readiness; drive ACCEPTED/FETCHING/VERIFYING/LOADING/WARMING/READY with bounded FAILED/CANCELLED/EXPIRED outcomes; use optional collaboration events only as notification and the validated status snapshot for recovery; use existing lease, stage, readiness, journal, commit/abort, and orphan-recovery owners; reject stale/replayed/cross-role status, stale boot epochs, wrong artifacts/adapters/roles, partial readiness, and model-runner invocation before the complete all-role readiness barrier; prove no new generic base message, parallel Targeted status service, or second state machine was added (FR-016-FR-021, FR-023, FR-032, FR-034; SC-003, SC-008)

**Checkpoint**: One restart-safe operation can turn selected responsibilities
into exact certified readiness, independent of how it was invoked.

## Phase 3: User Story 1 - Cold Request to Result (Priority: P1)

**Goal**: The primary user path works without an explicit deployment call.

**Independent Test**: A cold signed definition reaches a typed result through
one request handle in local and MiniNDN fixtures.

- [x] T004 [US1] Implement the complete shared distributed request path and optional prewarm reuse in `app_sdk/client.py`, `app_sdk/application.py`, and `app_sdk/deployment.py`: make `InferenceApplication.define` canonicalize/sign typed intent; make the sole `InferenceClient.request(deployment: RequestableDeployment, ...)` implementation accept and revalidate signed definitions/refs/handles, durably publish/freeze intent, run the bounded decision graph through the authorized coordinator, resolve one immutable model/graph/role plan revision, collect offers, select/assign Providers, bind the existing responsibility/membership/lease execution certificate, invoke the T003 ensure operation, pass the all-role readiness barrier, execute, and return one `InferenceRequestHandle`; make `InferenceApplication.request` delegate strictly to it; keep the DI-facing surface at `status`, authoritative `deployment_status`, `events`, `wait`, result/result_async parity, idempotent cancel, and restart rebind while reconciling T003's low-level generic query/watch/wait internally; make `deploy(definition)` and `wait_until_active` optional prewarming that call the identical ensure operation and may publish an ACTIVE record; make ACTIVE request revalidate/reuse readiness; fail closed for unauthorized/modified definitions, invalid policy outputs, forged/stale/replayed progress, stale readiness, progress loss, partial-role failure, deadline expiry, cancel-before/after certification, crash/abort, replay, and concurrent revision races in `tests/python/test_ndnsf_di_user_journey.py` and request-handle tests (FR-001-FR-007, FR-010-FR-015, FR-018-FR-022, FR-032, FR-034; SC-001, SC-003, SC-005, SC-008)

**Checkpoint**: `request = application.request(definition, ...)` followed by
`request.result()` is the baseline workflow; predeployment changes latency, not
semantics or correctness.

## Phase 4: User Story 2 - Remote ACTIVE/ON_DEMAND Use (Priority: P1)

**Goal**: A non-creator can invoke authorized cold intent or a warm immutable
revision without gaining definition or lifecycle authority.

- [x] T005 [US2] Implement `publish_definition`, typed `InferenceClient.deployments`, `DeploymentDefinitionRef`, `DeploymentRef`, and `DeploymentSummary.deployment`: bind every ON_DEMAND reference to its Application-authorized deployment coordinator; treat NDNSD fields only as untrusted name/digest hints; fetch exact signed definition Data for ON_DEMAND deployments and additionally validate lifecycle owner, activation record/certificate, freshness, epoch, and revocation/rollover fence for ACTIVE deployments; let the same `InferenceClient.request` accept either an in-memory signed `DeploymentDefinition` or its reference and route cold realization to that coordinator so requester space cannot author/re-sign definitions, load policy code, alter intent, or gain lifecycle authority; preserve immutable revision binding for ACTIVE deployments and durable reference rebind for both; cover signature parity between Application/Client calls, malformed/unsigned/expired/unauthorized definition, forged/coordinator-mismatched hint, unavailable coordinator, stale/draining/revoked activation, duplicate/race ordering, cold remote preparation, warm reuse, serialization, and restart in `tests/python/test_ndnsf_di_deployment_catalog.py` and the MiniNDN requester fixture (FR-005-FR-010, FR-016-FR-022, FR-032-FR-034; SC-002-SC-003, SC-005, SC-008)

**Checkpoint**: Discovery returns one typed requestable deployment whose authority and
readiness semantics are explicit.

## Phase 5: User Story 3 - Safe Extension Surfaces (Priority: P1)

**Goal**: Provider and optimization teams depend only on explicit role-specific
contracts.

- [x] T006 [P] [US3] Consolidate the provider application surface around `InferenceProvider.serve` and `run/stop`; remove direct lifecycle methods from serving; construct stage/activate/drain/delete and signed DI readiness/checkpoint actions only through the separately credentialed advanced `ProviderAdminPort`; allow serving/preparation code to report observational generic status through the T003 NDNSF reporter without gaining lifecycle authority; integrate provider-side offer/status/readiness delegates while keeping generic Collaboration progress separate from DI readiness and `RunnerAdapterRegistry` independent; and close provider import, optional-backend, registration, pre-context/context status, serving-without-admin, unauthorized-admin, authenticated lifecycle, and real startup preflight tests without changing base NDNSF Request/ACK/Selection/Response semantics (FR-016-FR-021, FR-023-FR-024, FR-028-FR-030, FR-032, FR-034; SC-004-SC-005, SC-008-SC-009)

- [x] T007 [P] [US3] Strengthen the external optimizer SDK without redesigning algorithms: replace shared `PolicyRequest`/`PolicyResult` aliases with ten dedicated typed request/result dataclass pairs declaring units, bounded candidates, required upstream evidence, and seam-specific outputs; add `OptimizationSuite.defaults().replace(...).build(...)`; derive suite identity from ordered public descriptors/configuration; validate epochs/budgets/candidate bounds/evidence and explicit fallback; preserve all ten seams plus independent RunnerAdapter; represent READY preference only as optional provider-selection input/default policy behavior, never a protocol prerequisite; update the external fixture and pass public import/type/determinism/failure/isolation tests (FR-022, FR-024-FR-029, FR-034; SC-004-SC-006)

**Checkpoint**: External optimization can exploit warm Providers without Core
depending on one ranking algorithm, and serving remains least-authority.

## Phase 6: User Story 4 - Bounded Migration and Executable Documentation

- [x] T008 [US4] Implement compatibility delegates in `ndnsf_distributed_inference/compatibility/`: map maintained root calls, result-returning helpers, legacy local/network `submit`, lifecycle stream, raw discovery, `serve_service`, and direct provider lifecycle surfaces to canonical owners; preserve old explicit deploy-before-submit behavior while allowing migration to direct `request`; emit bounded actionable warnings without sensitive data; record usage through existing observability; prohibit removal in this feature; and prove warning-once plus normalized result/token/fence/readiness/receipt parity in `tests/python/test_ndnsf_di_api_compatibility.py` (FR-014-FR-015, FR-023, FR-029-FR-032, FR-034; SC-005, SC-009)

- [x] T009 [US4] Rewrite English and Chinese README/API-reference/examples together around cold creator request, preparation observation, remote ON_DEMAND/ACTIVE invocation, optional prewarm, provider, optimizer, recovery, advanced operations, and migration; extract and execute every primary snippet against the installed package with explicit production state/key configuration and CPU-only/optional-dependency-absent variants; migrate maintained repository examples to canonical imports/signatures while preserving compatibility fixtures; and publish an API-diff/migration table generated from the approved manifest (FR-004, FR-008-FR-015, FR-019-FR-022, FR-030-FR-032; SC-001-SC-003, SC-007, SC-009)

## Phase 7: Distributed Acceptance and Completion Audit

- [x] T010 [US1] Run one candidate-bound acceptance campaign comprising a deterministic local correctness/security matrix, the exact-name signed-catalog MiniNDN fixture, and the multi-Provider readiness/execution MiniNDN fixture. Across those complementary fixtures cover controller, Application creator/signer, definition-bound deployment coordinator, remote requester, cold ON_DEMAND and signed ACTIVE discovery, typed offers, Selection responsibility, all preparation stages, already-ready reuse, optional prewarm, generic query/watch/wait and snapshot recovery, forged/stale/replayed/cross-role/epoch/readiness/artifact/adapter/definition/activation/receipt negatives, partial-role failure, coordinator mismatch/unavailability, deadline, cancellation, restart, rollover/revocation, fence rejection, orphan cleanup, and compatibility parity. Preserve exact commands, candidate identity, cell counts, measured failures, and fixture ownership in `evidence/`; then run strict Spec Kit structure, cross-artifact consistency, task-cohesion, CodeGraph ownership, security/migration, and post-implementation semantic audit and write `completion-summary.md` plus final `audit-report.md` without claiming Docker, iTiger, GPU, Qwen performance, or READY-first latency improvement (FR-001-FR-034; SC-001-SC-010)

## Dependencies & Execution Order

```text
T001 -> T002 -> T003 -> T004 -> T005 ───────────────┐
                    ├──────> T006 ──────────────────┤
T001 ───────────────└──────> T007 ──────────────────┤
T002-T007 -> T008 -> T009 -> T010 <─────────────────┘
```

- T003 precedes request behavior because ACK/Selection/readiness/status meaning
  must be fixed before `request` can safely hide deployment.
- T004 proves the primary cold creator path before T005 adds remote discovery.
- T006 and T007 retain separate provider and optimizer ownership; T007 can run
  after T001, while T006 integrates the T003 provider contracts.
- Compatibility and docs follow stable canonical behavior.
- T010 is the only authoritative full-network completion gate.

## Minimum Reviewable Slice

T001-T004 deliver direct request, observable preparation, exact readiness,
execution, durable result, and optional prewarm reuse for an Application
creator. T005 adds the remote requester. Neither T006-T009 nor performance
optimization is allowed to weaken this correctness slice.

## Task-Cohesion Review

- Mechanically fragmented task chains detected: 0.
- Remaining coalescing opportunities: 0.
- Each task closes one independently reviewable behavior and includes contract,
  implementation, focused tests, and evidence.
- Request-time preparation remains one task chain because ACK semantics,
  responsibility assignment, progress recovery, readiness, and execution
  certification form one inseparable correctness boundary.
- T003 keeps the generic NDNSF status substrate and its DI projection in one
  vertical task: splitting “Core API,” “DI mapping,” “tests,” and “evidence”
  would create independently incomplete work rather than reviewable behavior.

