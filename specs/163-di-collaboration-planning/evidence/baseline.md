# T001 V1 Migration and Ownership Baseline

**Feature**: Spec 163, Pluggable DI Collaboration Planning  
**Snapshot date**: 2026-07-28  
**Status**: immutable pre-implementation baseline  
**Change boundary**: documentation only; no C++, Python, wire-format, test, or
runtime behavior was changed while producing this baseline.

## 1. Baseline purpose and authority

This document records the code that exists before Spec 163 implementation. It
is migration evidence, not approval of the current ownership boundary and not
evidence that the deferred-planning API already exists.

The authoritative V2 direction is:

1. base NDNSF owns the generic Request/ACK/Selection/Response carrier,
   permission and token checks, immutable ACK closure, an opaque same-invocation
   plan commit, and a generic crash-safe participant transaction seam;
2. NDNSF-DI owns model interpretation, graph extraction, splitting, Provider
   resource interpretation, role assignment, artifact/state policy,
   preparation, dependency execution, result completeness, and recovery;
3. the current roles-before-Request path is retained only as
   `PREPLANNED` compatibility;
4. no `Deployment*`, model/GPU, role/artifact, preparation/readiness,
   tensor/DAG, result, or DI recovery behavior currently located in Core is
   allowed to become V2 authority merely because it already exists.

## 2. Current Collaboration carrier

### 2.1 Public surfaces

| Surface | Current contract | Consequence |
|---|---|---|
| C++ requester | `ServiceUser::RequestCollaboration(service, initialRequest, CollaborationPlan, response, timeout, requestId)` in `ndn-service-framework/ServiceUser.hpp` and `ServiceUser.cpp:4110` | A complete `CollaborationPlan` is passed before Request publication. |
| C++ plan | `CollaborationPlan` contains `roles`, `keyScopes`, `dependencies`, `ackCollectionTimeMs`, `timeoutMs`, and `participantSelector` (`ServiceUser.hpp:126`) | Current lifecycle is preplanned; it cannot let a DI strategy derive the split and roles from the closed ACK set. |
| Python requester | `ServiceUser.request_collaboration(...)` and `_async(...)` in `pythonWrapper/ndnsf/service.py:1596,1664` | `roles` and `key_scopes` are required keyword arguments; dependencies and artifact/scope/role maps are also encoded before entering native Core. |
| Python/native selector | `RoleAssignmentSelectionPolicy` in `pythonWrapper/src/ndnsf/_ndnsf.cpp:1712` | It parses roles advertised in ACK payloads, applies optional role-to-Provider preferences, and builds assignment text from the caller-authored role list. |
| ACK observation | Python `ack_observer` is converted to immutable value snapshots before the selector (`service.py:1625-1645`) | It is observational, returns no decision, and cannot safely serve as the V2 commit authority. |

`RequestCollaboration()` deliberately stores `AllSelected` as the generic ACK
strategy so the legacy `RandomSelection` 100 ms callback does not race the
collaboration selector (`ServiceUser.cpp:4133-4136`). ACKs are collected until
timeout or until a valid coverage condition is satisfied. The historical
`collaborationAckRoleCoverageSatisfied()` (`ServiceUser.cpp:5365`) invokes the
participant selector against the predeclared `PREPLANNED` role list. The
deferred carrier now also exposes an application-owned
`CollaborationAckCoverageHandler`: it receives authenticated candidates and may
close only when a bounded capability hint (for example, all required stage
roles) is covered. It cannot select Providers, mutate the plan, or bypass the
immutable `ACK_CLOSED` snapshot; DI graph inspection, splitting, and assignment
remain post-closure.

### 2.2 `PREPLANNED` compatibility mapping

| Existing V1 action | V2 compatibility meaning |
|---|---|
| Caller constructs `CollaborationPlan` / supplies Python roles, key scopes, and dependencies before Request | `begin_collaboration(mode=PREPLANNED, opaque_plan=...)` |
| Request is published and positive ACKs are collected | Same generic Request and ACK-closure state machine as `DEFERRED` |
| Predeclared role coverage may close ACK collection early | Existing selector path in `PREPLANNED`; `DEFERRED` may use only an application-owned validated coverage predicate, never a precomputed plan |
| Existing participant selector chooses Providers and assignment payloads | Compatibility adapter produces the one opaque committed plan |
| Core sends final Selection to each selected Provider | Same final Selection transport and one-time ProviderToken rule |

Source compatibility does not mean authority compatibility. New Spec 163 DI
calls default to `DEFERRED`: publish one Request, close one immutable ACK
snapshot, let trusted NDNSF-DI compute/materialize its plan, then call
`commit_plan()` on that same invocation. `PREPLANNED` cannot be used as evidence
that post-ACK planning exists.

## 3. Current provider-side generic primitives

`ServiceProvider::CollaborationContext` in
`ndn-service-framework/ServiceProvider.hpp` and its Python projection in
`pythonWrapper/ndnsf/service.py:490` currently expose:

| Primitive family | Current operations | V2 ownership |
|---|---|---|
| Identity/assignment | session/request identity, local Provider, role, opaque assignment | Core retains generic identity and opaque assignment; NDNSF-DI interprets roles and DI fields. |
| Object access | `hasArtifact`, `fetchArtifact`, `getArtifact`, encrypted exact-name large-data fetch | Retain as generic object transport; “model shard,” tensor, adapter, and state meanings stay in NDNSF-DI. |
| Object publication | `publish`, `publishLarge`, `publishLargeNamed`, large-reference publication | Retain generic scoped/exact-name bytes and references. |
| Dependency observation | one-item and predicate waits/subscriptions, large-object/reference fetch | Retain generic data events; NDNSF-DI owns DAG validation and eligibility. |
| Progress | `report_operation_status()` and requester snapshot/watch/wait | Retain generic authenticated status; status is evidence, never authorization. |
| Terminal output | `publish_final_response()` / Response publication and `fail()` | Retain generic Response transport; NDNSF-DI owns completeness and result-envelope validation before publication. |

These are sufficient building blocks for V2. They do not justify a second
NDNSF-DI transport or a Core-side inference state machine.

## 4. Exact collaboration caller inventory

### 4.1 Maintained source callers

| Caller | Current use | Classification and migration owner |
|---|---|---|
| `examples/Payment_User.cpp:275` | Hand-authored payment roles, key scopes, artifacts, selector | Generic fixed-plan example; preserve under `PREPLANNED`. Core example owner. |
| `examples/AI_User.cpp:416` | Hand-authored AI roles, dependencies, artifact names, selector | Legacy AI fixed-plan example; preserve as compatibility evidence, never V2 DI authority. |
| `NDNSF-DistributedInference/.../client.py:541` (`_request_plan_session`) | Converts an already-built `DistributedInferencePlan` to roles/dependencies/artifact references | NDNSF-DI V1 fixed-plan caller; migrate canonical Spec 163 path to `DEFERRED`. |
| `NDNSF-DistributedInference/.../client.py:623` (`infer_deployed`) | Sends explicit roles/dependencies plus optional assignment preference | NDNSF-DI predeployed compatibility path; keep explicitly named compatibility only. |
| `NDNSF-DistributedInference/.../app_sdk/coordinator.py:277` | One predeclared `coordinator` role | Coordinator-only private lifecycle bootstrap; remove from V2 authority and route the full invocation through deferred Collaboration. |
| `NDNSF-DistributedInference/.../app_sdk/facades.py:275` | Authenticated one-role execution-control operation | Fixed control collaboration; preserve as `PREPLANNED` unless a later task folds it into the V2 attempt transaction. |
| `NDNSF-DistributedInference/.../deployment.py:1175` | Compatibility helper injects an `AssignmentContext`, then delegates | Compatibility-only wrapper; must not be the canonical application call. |
| `examples/.../native_di_tracer/user_driver.py:707` | Synchronous multi-role tensor-bundle request with ACK observation and optional preference | V1 experiment caller; freeze as prior evidence and port only in a new Spec 163 fixture. |
| same file `:840` | One-role lifecycle control request | `PREPLANNED` control compatibility. |
| same file `:947` | Async cancellation scenario over a fixed plan | Frozen V1 experiment caller. |
| same file `:1105,1266` | Async concurrent/rate-series fixed-plan requests | Frozen V1 benchmark caller; do not rewrite historical results. |

`app_sdk/gui.py:2516` calls `request_collaboration_with_deployment()` indirectly;
it is an application entry to the compatibility wrapper, not a separate carrier
implementation. `app_sdk/gui.py` runtime capability checks are likewise not
collaboration calls.

### 4.2 Test doubles and generated copies

- `tests/python/test_ndnsf_di_assignment_context_compatibility.py` owns the
  compatibility-helper delegation contract.
- `tests/python/test_ndnsf_di_deployment_catalog.py` owns the coordinator
  routing/realization compatibility behavior and includes an async
  Collaboration fake.
- `NDNSF-DistributedInference/pythonWrapper/build/` and other build/package
  mirrors are generated artifacts, not editable source authorities.

No test currently proves the proposed `DEFERRED` begin/ACK-closed/commit
lifecycle, because that API does not yet exist.

## 5. Ten-policy V1 port inventory

`sdk/contracts.py` declares exactly ten `POLICY_KINDS`, composed by
`sdk/suite.py::OptimizationSuite`:

| Kind | Port method | Current default/implementation owner | V2 disposition |
|---|---|---|---|
| `model_variant` | `propose(ModelVariantRequest)` | `planner/defaults.py`, `model_variant_policy.py` | Compatibility input to one joint placement strategy. |
| `partition` | `propose(PartitionRequest)` | current partition/split planning | Adapt graph-derived candidates; no independent final authority. |
| `deployment` | `propose(DeploymentRequest)` | `deployment_policy.py` | Compatibility only; canonical application call has no deployment prerequisite. |
| `provider_assignment` | `propose(ProviderAssignmentRequest)` | `provider_assignment_policy.py` | Fold into joint post-ACK placement. |
| `scheduling` | `dispatch(SchedulingRequest)` | `scheduling_policy.py` | Preserve as downstream/local scheduling seam, not plan authority. |
| `admission` | `admit(AdmissionRequest)` | `admission_policy.py` | Preserve bounded local admission; ACK offer remains authenticated input. |
| `execution_tuning` | `propose(ExecutionTuningRequest)` | `execution_tuning_policy.py` | Adapter/backend-local tuning. |
| `cache` | `propose(CacheRequest)` | `cache_policy.py` | Adapt to immutable artifact cache and separate derived-state cache policy. |
| `recovery` | `transition(RecoveryRequest)` | `recovery_policy.py` | NDNSF-DI attempt/generation recovery only. |
| `execution_target` | `propose(ExecutionTargetRequest)` | `execution_target_policy.py` | Fold capability/backend target into validated placement output. |

`app_sdk/client.py:203::APPClient.decide(requests)` directly calls
`engine.run_decision_graph(requests)`. It remains a non-authoritative
compatibility adapter; it is not the application invocation and must not cause
network, repository, or GPU side effects during V2 strategy evaluation.

## 6. Selection, token, timer, and cleanup baseline

### 6.1 One-token ACK/Selection path

On a positive ACK, `ServiceProvider` stores the pending Request and creates or
reuses one pending ProviderToken (`ServiceProvider.cpp:3184-3205`). The ACK
carries that token. Final Selection is Provider-targeted and must present the
matching token. The accepted Selection path records the consumed token hash,
removes pending Request/token/lease state, and rejects replay
(`ServiceProvider.cpp:9145-9178`). Spec 163 retains this generic one-time token
property. A Provider assigned multiple roles receives one complete role tuple
in its one Selection; it does not require one ProviderToken per role.

Current regression ownership includes:

- `tests/unit-tests/generic-dynamic-api-tokens-replay.t.cpp`: token handshake,
  compact Provider-bound proof, replay, expiry, and cleanup;
- `tests/unit-tests/generic-dynamic-api-selection.t.cpp`: ACK-window and
  Selection timing plus R1 decision/receipt behavior;
- `tests/unit-tests/generic-dynamic-api-workers.t.cpp`: AllSelected and
  Collaboration final-response cleanup;
- `tests/unit-tests/generic-dynamic-api-crypto-auth.t.cpp`: ACK token
  confidentiality, recipient assignment, and tamper binding.

### 6.2 Overlapping fixed cleanup callbacks

The same Provider `pendingKey` can receive two fixed-TTL cleanup schedules:

1. after V2 Request replay/duplicate state is installed,
   `schedulePendingRequestCleanup(pendingKey)` is called at
   `ServiceProvider.cpp:7016`;
2. after a positive ACK stores `pendingRequests[pendingKey]`, the same function
   is called again at `ServiceProvider.cpp:3195` (and the alternate normal ACK
   path also schedules at `:7159`).

`schedulePendingRequestCleanup()` (`ServiceProvider.cpp:6327`) does not retain
or replace an event handle. Each scheduled callback may add a further
`m_pendingRequestTimeoutGrace` callback before
`expirePendingRequestState()`. State erasure is guarded and therefore normally
idempotent, but deadline ownership is duplicated and cannot support the V2
claim of one monotonic invocation budget.

V2 disposition: T008 must establish one generic deadline/transaction owner and
cancel or make obsolete every superseded callback. T009 then derives DI
attempt/resource cleanup from that deadline without adding a parallel Core
timer. The current callbacks remain frozen V1 behavior until those tasks.

The requester separately owns one cancelable request timeout
(`ServiceUser.cpp:2117`) plus ACK-window callbacks
(`ServiceUser.cpp:5800`); these are different lifecycle purposes and are not
misreported here as the Provider duplicate.

## 7. Current synchronous preparation path

`core/deployment_control.py:462::SelectionGatedProvider` is NDNSF-DI code, but
its `select()` method (`:511`) performs `VERIFYING -> LOADING -> WARMING ->
READY` synchronously inside the Selection call. It releases on preparation
failure and refuses activation unless READY. This is a V1 compatibility
baseline only.

Core also contains a parallel legacy deployment branch:

- `ServiceProvider.hpp:260` declares `DeploymentPrepareHandler`,
  `ProviderReadyPublisher`, and `acceptExecutionActivate()`;
- `ServiceProvider.cpp:9223-9331` requires `DeploymentPlan`, calls the
  preparation handler synchronously, and handles Provider readiness;
- `ServiceUser.cpp:6930-7500` synthesizes/stores deployment plans, validates
  all Provider-ready messages, and publishes `ExecutionActivateMessage`;
- `ServiceUser.hpp:917-919` stores `deploymentPlan`,
  `deploymentReadyByMember`, and `deploymentActivationSent`.

This Core branch contradicts the V2 ownership and data-driven execution model.
It is quarantined legacy behavior. V2 preparation is NDNSF-DI-local and
asynchronous after final Selection; a role starts when its own local readiness
and verified direct-input predicates are true, without a global ReadySet or
ExecutionActivate barrier.

## 8. Complete Core/Python DI semantic-debt inventory

### 8.1 Message and branch inventory

`NDNSFMessages.hpp:65-86,157-194` currently defines the following
deployment/DI-originated message family under base NDNSF:

`DeploymentIntent`, `ProviderCapabilityOffer`, `DeploymentPlan`,
`ProviderReadyMessage`, `ReadyAcknowledgement`, `ExecutionActivateMessage`,
`SecureStatusQuery`, `SecureStatusSnapshot`, `RequestCapabilities`,
`EncryptedRequestInput`, `SelectionInputKeyOffer`, `SelectionInputKeyGrant`,
`ReservationLease`, `SelectionDecision`, `SelectionDecisionReceipt`,
`RecipientEncryptedAssignment`, `StageInputEvidence`, `StageAbort`, and
`SelectionDecisionTombstone`.

They share `DeploymentControlMessage`, whose fields are opaque to the codec but
whose names, branch conditions, handlers, readiness aggregation, and activation
behavior are not all generic in the current runtime. Python binds this family
as `NativeDeployment*`, `NativeProviderReady*`, reservation/decision, input,
assignment, and stage types at
`pythonWrapper/src/ndnsf/_ndnsf.cpp:3926-3944`.

The following current handlers/branches are also in the migration inventory:

- Request `deployment_intent` conversion and optional submission in
  `_ndnsf.cpp:304,2759-2795,3416-3470`;
- `RoleAssignmentSelectionPolicy`, role/artifact/provisioning assignment
  encoding, and pre-request plan construction in `_ndnsf.cpp:1712-1860,
  3134-3368`;
- Python provider deployment-prepare and R1 reservation decision callbacks in
  `_ndnsf.cpp:2231-2305`;
- Core Request/Selection serialization of DeploymentIntent/DeploymentPlan in
  `NDNSFMessages.cpp`;
- requester plan synthesis, reservation decisions, readiness cover, and
  activation in `ServiceUser.cpp:5416-5675,6930-7500`;
- Provider DI-reservation capability branch, deployment-plan branch,
  preparation callback, readiness publication, and activation gate in
  `ServiceProvider.cpp:3160-3200,7991-8010,8980-9331`;
- `runtimeGpuUtilization` in generic Provider runtime hints
  (`ServiceProvider.cpp:1655,1697`) and Python constants
  `GPU_BUSY`/`MODEL_UNAVAILABLE` (`pythonWrapper/ndnsf/service.py:31-32`).

### 8.2 Semantic disposition by required domain

| Domain | Current Core/Python representation | Required V2 disposition |
|---|---|---|
| Model | `DeploymentIntent` opaque fields, Python `deployment_intent`, `MODEL_UNAVAILABLE`; comments/hooks name model preparation | Model identity and availability move to digest-pinned NDNSF-DI `ModelRef`, adapter, and offer codecs. Core sees opaque bytes only. Remove model wording/branches from V2 Core surface; retain finite V1 compatibility quarantine. |
| GPU | `RuntimeHint.gpuUtilization`, `runtimeGpuUtilization`, `GPU_BUSY`, reservation fields interpreted by DI-adjacent branches | GPU capacity, utilization, and memory are signed `DIProviderOfferV2` fields interpreted only by NDNSF-DI. Generic Core may carry opaque telemetry but cannot select or validate GPU semantics. |
| Role | `CollaborationRoleSpec`, `SelectedParticipant`, Python role parsing/preferences, `roleProvider.*` assignment text | Generic Collaboration may carry opaque participant labels, but graph roles and feasibility belong to the committed NDNSF-DI plan. `PREPLANNED` adapter retains old role fields. |
| Artifact | `requiredArtifact`, `assignedArtifact`, `artifactDataName`, artifact fetch helpers | Retain generic exact-name object primitives; shard/adapter/state identity, digest, publication, and residency policy move to NDNSF-DI. |
| Preparation | dynamic provisioning flags/timeouts, `DeploymentPrepareHandler`, ProviderReady cover, `ExecutionActivateMessage`, synchronous `SelectionGatedProvider.select()` | Quarantine current V1 path. V2 uses DI-local asynchronous verification/load/warm and per-role local-ready evidence; no Core all-ready activation. |
| Tensor | Core collaboration payload/large-object bytes and `StageInputEvidence` | Core retains opaque segmented/scoped bytes. NDNSF-DI adapter owns tensor schema, shape/dtype/sequence, direct-input evidence, and validation. |
| DAG | `CollaborationDependency` transport fields; stage evidence/abort types | Generic dependency notification may remain opaque; NDNSF-DI owns ONNX-derived graph validation, role dependencies, fan-in/out, and eligibility. No Core inference DAG scheduler. |
| Result | generic Response plus deployment/status summaries | Core retains authenticated Response and CAS/terminal transport. NDNSF-DI validates `DIResultEnvelopeV2`, sinks, provenance, completeness, and adapter-owned task result before allowing final Response. |
| Recovery | Core pending-state expiry, R1 decision tombstones/stage abort, DI recovery policies and cancellation paths | Core owns generic token/transaction recovery only. NDNSF-DI owns invocation/attempt generations, resource compensation, stage abort propagation, result fencing, and optional derived-state adoption. |

The generic names that may be reusable (`RequestCapabilities`, encrypted input,
recipient-encrypted opaque assignment, decision/receipt/tombstone) still require
the T008 non-DI fixture and ownership audit. Until then they are V1 components,
not automatically accepted V2 Core primitives.

## 9. Existing tests and evidence ownership

| Behavior | Current owner | Baseline meaning |
|---|---|---|
| Deployment-control codec and bounds | `tests/unit-tests/generic-dynamic-api-deployment-control.t.cpp`; `tests/python/test_spec129_binding_contract.py` | V1/R1 compatibility evidence only. |
| Selection timing, reservation decision, receipt | `generic-dynamic-api-selection.t.cpp`; `test_spec129_reservation_book.py` | Preserve; cannot prove deferred plan commit. |
| Token/replay/cleanup | `generic-dynamic-api-tokens-replay.t.cpp` | Generic security regression to retain. |
| Recipient encryption and assignment binding | `generic-dynamic-api-crypto-auth.t.cpp` | Generic security regression to retain. |
| Synchronous Selection preparation | `test_spec129_selection_gated_core.py` | Frozen predecessor behavior, explicitly not V2 preparation semantics. |
| DI contracts, ready cover, dependency execution | `test_ndnsf_di_core_contracts.py`, `test_ndnsf_di_execution_consistency.py`, `test_ndnsf_di_core_execution.py` | V1 migration coverage; complete-ready activation assertions must not define V2 authority. |
| Assignment-context compatibility | `test_ndnsf_di_assignment_context_compatibility.py` | `PREPLANNED` compatibility coverage. |
| Cancellation/recovery | `test_ndnsf_di_request_cancellation.py` | Preserve as prior behavior; V2 needs generation/transaction-specific cases. |

CodeGraph found no direct current test owner for the exact public
`RequestCollaboration()`/Python `AckCandidate` surface. Text inventory found
indirect Collaboration behavior in the worker, token, selection, crypto, DI
catalog, and compatibility tests above. T004 must add direct
`PREPLANNED`-compatibility and `DEFERRED` state-machine tests rather than
claiming this gap is already closed.

## 10. Frozen evidence preservation

- Spec 116 remains closed. Its
  `specs/116-ndnsf-di-user-api-coherence/evidence/verification.md`,
  completion summary, audit, tests, and any referenced results are immutable
  predecessor evidence. They may be cited, not edited or reclassified as Spec
  163 acceptance.
- Spec 129 remains frozen by
  `specs/129-selection-gated-deployment/FROZEN.md`: 12/12 cells accepted once.
  Its result directory, manifests, hashes, negative rows, implementation
  evidence, and complete-ready/activation history are not rerun, tuned,
  overwritten, or presented as V2 success.
- Spec 163 creates new tests and a new MiniNDN/Qwen3-0.6B result namespace only
  at T013. TigerCluster and large-model results remain out of scope unless the
  user separately authorizes them.

## 11. T001 acceptance record

- [x] Zero source/runtime behavior change.
- [x] C++ and Python carrier signatures and pre-request plan requirement mapped.
- [x] ACK observer, selector, early-coverage behavior, and one-token flow mapped.
- [x] Maintained callers, indirect callers, test doubles, and generated mirrors classified.
- [x] Provider data/status/Response primitives mapped.
- [x] Ten policy ports and `APPClient.decide()` compatibility owner mapped.
- [x] `SelectionGatedProvider.select()` and the parallel Core preparation path mapped.
- [x] Overlapping Provider pending-request cleanup schedules identified by exact call sites.
- [x] Base NDNSF/Python DI-originated messages, bindings, handlers, and branches inventoried.
- [x] Model, GPU, role, artifact, preparation, tensor, DAG, result, and recovery dispositions recorded.
- [x] `PREPLANNED` mapping and `DEFERRED` non-equivalence stated.
- [x] Spec 116/129 evidence preservation rule stated.
- [x] No legacy Core DI path is designated V2 authority.
