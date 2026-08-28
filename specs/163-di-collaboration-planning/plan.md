# Implementation Plan: Pluggable DI Collaboration Planning

**Branch**: `Experimental` | **Date**: 2026-07-28 |
**Spec**: [spec.md](spec.md)

**Input**: Dissertation-scoped architecture change defined in
`specs/163-di-collaboration-planning/spec.md`

## Summary

Replace the current fixed-plan-before-ACK path and separate partition/provider
policy ports with one validated, externally replaceable
`ModelPlacementStrategy`. A model-specific splitter supplies common graph units,
runtime-peak estimates, and split constraints; a built-in pre-split-first
strategy jointly chooses an existing exact manifest or derives a new
`SplitSpecification` and provider-role assignment from validated ACK snapshots.
The canonical carrier is NDNSF's existing generic distributed-collaboration
API. NDNSF adds a DI-opaque
`begin_collaboration -> ACK_CLOSED -> commit_plan` lifecycle so roles and
dependencies may be supplied after capability discovery; the current
roles/dependencies-before-Request form remains a `PREPLANNED` compatibility
mode, while Spec 163 defaults to `DEFERRED`.
Operator-trusted NDNSF-DI code materializes a generated split, publishes its
immutable signed manifest through NDNSF-DistributedRepo, validates the plan,
and only then sends final Selection. A generic positive NDNSF
ACK carries an opaque signed `DIProviderOfferV2`; only NDNSF-DI interprets it as
the Provider's binding willingness to accept a compatible role set whose
aggregate requirements fit its advertised capability and GPU-memory envelope.
One final Selection per selected Provider carries an opaque
`DISelectionAssignmentV2`, consumes its existing ProviderToken and optional
generic lease once, and starts asynchronous role-local preparation. The
Provider records a mandatory per-Provider `DISelectionAcceptanceV2` for
crash-safe retry and audit; this is linearization evidence, not a second
role-negotiation round or global acceptance cover. One public invocation may
contain bounded attempts, while each attempt has at most one Request, one ACK
closure, and one immutable plan; an attempt that reaches `REQUEST_PUBLISHED`
has exactly one Request. The dependency-driven executor starts each role
independently when Selection, local preparation, and all authenticated direct
inputs are ready.

The public request is model-family-neutral. A digest-pinned
`ModelFamilyAdapter` composes pure graph/split, task-I/O, and state ports with a
separately trusted runner port. Models without inspectable internals use an
atomic one-node graph. A general `InferenceStateContract` distinguishes
request-scoped state from optional reusable derived state; exact prefix KV is
the first LLM-specific profile, remains Provider-local by default, and is
bound into the plan and Selection without adding LLM semantics to NDNSF Core.

This feature supplies the foundation and extension API required by the
dissertation. It does not attempt universal globally optimal partitioning or
all model-parallel algorithms.

## Technical Context

**Language/Version**: Python 3.10+ application/DI SDK and tests; C++17/ndn-cxx
only where the existing generic runtime requires an API seam

**Primary Dependencies**: base NDNSF distributed-collaboration
`RequestCollaboration`/`request_collaboration`, `CollaborationPlan`, Selection,
`CollaborationContext`, collaboration large-object/data, status and Response
transport and security; NDN-SVS, NAC-ABE; NDNSF-DI application SDK and
execution/deployment contracts, NDNSF-DistributedRepo, optional model-family
ONNX, PyTorch, container, LLM, vision, speech, and diffusion adapters

**Storage**: Content-addressed immutable deployment definitions, pre-split
manifests, artifact and input/output manifests, bounded encrypted-at-rest
control journals, tombstones, Provider-local bounded derived-state caches, and retained
plan/ACK-offer/Selection-acceptance/preparation/result evidence

**Testing**: Python unit/contract/negative/state-machine tests, a
property/model-based concurrent-history oracle, focused C++/Python generic-Core
security regressions where touched, a non-DI Core fixture, local Docker
fake-payload closed loop, then MiniNDN with a frozen `Qwen/Qwen3-0.6B`
`ModelRef` containing exact content and semantics digests;
TigerCluster/large-model jobs only under explicit
separate authorization

**Target Platform**: Linux NDNSF-DI applications and Providers. MiniNDN is the
default network-validation environment; CUDA-backed cache claims are validated
only when local CUDA is available and otherwise remain deferred.

**Project Type**: Framework library plus application SDK, model adapters,
provider runtime adapters, and reproducible network experiments

**Performance Goals**: Policy work is bounded by explicit candidate/time
budgets; planning, split, publication, fetch, disk/RAM/GPU preparation, and
execution timings are separately attributable. A paired cold/warm run reports
distributions; exact warm GPU reuse requires zero re-split, re-publication,
repository model-byte fetch, and GPU reload.

**Constraints**: Preserve NAC-ABE, permissions, one-time ProviderTokens,
signatures, replay protection, exact provider identity/boot epoch, one bounded
invocation/attempt deadline hierarchy, and frozen Spec 116/129 evidence. No
second preparation Request, `PreparationCommit`, complete all-role readiness
gate, global ReadySet/ExecutionActivate, cross-provider 2PC, model/GPU/role/
artifact/tensor semantics in base NDNSF, parallel DI collaboration protocol,
or live cluster actions.

**Scale/Scope**: One automatic plan spanning chain or fan-in/fan-out roles,
multiple Provider offers, one immutable pre-split catalog snapshot, bounded
fallback/compensation, and model-specific splitters behind one common output

## Constitution Check

### Pre-design gate

- **Canonical dynamic runtime**: PASS. The public invocation and each wire
  attempt use one existing generic NDNSF distributed-collaboration invocation,
  unified service name, Selection, `CollaborationContext`, data/status, and
  Response lifecycle. The new deferred-plan seam is generic and DI-opaque; DI
  messages remain versioned payload profiles, not new Core message kinds.
- **Security is part of the data path**: PASS. The one request uses
  the existing one-time ProviderToken, optional ACK admission lease, signature
  verification, provider permissions, recipient encryption, and replay
  protection. Only final Selection assigns concrete roles and grants input
  access; each role still waits for its own verified local model and inputs.
- **Layer ownership**: PASS as a design, not as current implementation.
  Base NDNSF owns the generic collaboration invocation, immutable ACK closure,
  one-shot plan commit, transport/authentication/tokens/opaque
  leases/Selection/`CollaborationContext`/data/status/Response/deadlines;
  NDNSF-DI exclusively owns model, split, GPU, role meaning,
  artifact, preparation, tensor-DAG, result, and recovery semantics. Existing
  Core DI-specific codecs and handlers are migration debt and cannot be reused
  as the V2 authority.
- **CodeGraph first**: PASS. Current optimizer, application request path,
  ONNX graph adapter, dependency executor, and Selection-gated loading behavior
  were inspected before this plan.
- **Spec-driven durable change**: PASS. The architecture is isolated in Spec
  163; frozen Spec 129 and unfinished Spec 162 are not rewritten as if they
  already implemented it.
- **Right-scope verification**: PASS. Pure API/state tests precede local Docker
  and MiniNDN. TigerCluster is excluded until later requalification.
- **Cohesive tasks**: PASS. Future tasks must close behavioral slices:
  strategy/catalog, ACK-offer/Selection, role-local dataflow/compensation, and
  end-to-end security evidence. They must not split tests, implementation, and
  evidence for the same behavior into separate bookkeeping tasks.

### Baseline design gate (historical)

PASS as a proposed capability, with implementation-time audit obligations. The
runtime inspected before implementation had the generic
`RequestCollaboration` carrier but required
roles/dependencies before Request; its ACK observer cannot return a plan and
its role-coverage check assumes predeclared roles. It also has one ACK
ProviderToken, short fixed pending-state cleanup, generic ACK/opaque-lease
support, and legacy Core-owned
CollaborationAssignment/preparation/status behavior. It did not yet satisfy
the generic deferred-plan seam, ownership boundary, binding DI offer,
crash-atomic multi-role acceptance,
mandatory acceptance record, invocation/attempt journal, object/result
contract, or barrier-free asynchronous dataflow in this plan. Implementation
must first migrate/quarantine legacy DI semantics out of base NDNSF, then close
those gaps while retaining generic deadline-bound pending state.

## Architecture and ownership

```text
Application API
  immutable model reference + task + typed input/options + objective/constraints
             |
             v
ModelFamilyAdapter
  task I/O + inference-state contract + trusted runner
             |
             v
Generic NDNSF begin_collaboration             canonical carrier
  one Request -> ACK collection -> immutable ACK_CLOSED snapshot
  mode=DEFERRED; no predeclared DI roles/dependencies
             |
             v
NDNSF-DI planning coordinator
  after ACK_CLOSED: graph/split and dependency planning
  validated per-ACK Provider planning views
    capacity + queue + RTT/bandwidth + exact model-shard residency
    + bounded opaque reusable-state evidence
  + read-only pre-split snapshot
  + model graph/runtime-peak constraints
             |
             v
ModelPlacementStrategy             external extension point
  selects manifest or SplitSpecification + provider-role assignment only
             |
             v
trusted validator + materializer + NDNSF-DistributedRepo publisher
  publish/seal immutable artifacts when new; then assemble DI plan
             |
             v
Generic NDNSF commit_plan on the same invocation
  generic roles + dependencies + Provider assignments + artifact names
  + opaque per-Provider DI assignment/state-reuse payloads
             |
             v
Existing final generic Selection    same requestId + attempt
  carries one complete DI Provider role tuple within offer
  crash-atomic token/lease/DI-ledger/tuple commit
  -> mandatory per-Provider acceptance evidence
  -> asynchronous local preparation + selected state revalidation/pin
             |
             v
Role-local dataflow gates            no all-role readiness barrier
  Selection + LocalReady(role) + InputsReady(role)
  -> admit role at most once -> publish authenticated object manifests
  -> existing CollaborationContext data/large-object/status primitives
  -> wake direct consumers -> validate one ResultContract
  -> one accepted DI result -> existing generic Response
```

Ownership rules:

- `ModelSplitter`: model graph analysis, graph-valid units, runtime-peak bounds,
  and common `SplitSpecification` support.
- `ModelFamilyAdapter`: composition of pure graph/split, task-I/O, and
  inference-state ports plus a separately trusted runner. Its planning ports
  own no network, repository, Selection, or device authority.
- `InferenceStateContract`: classifies mutable state, exact identity, resource
  cost, access domain, retention/migration, pin/revalidation, and cleanup.
- `SplitMaterializer` and `DistributedArtifactPublisher`: trusted side-effecting
  ports used only after decision validation; never passed to strategies. The
  publisher also resolves and revalidates exact existing publications.
- `ModelPlacementStrategy`: pure joint candidate/provider choice whose
  `PlacementDecision.artifact_preparation` explicitly selects `GENERATED` or
  `PRE_SPLIT`; free-form evidence never controls side effects.
- `PreSplitOps`: trusted catalog mutation.
- `PreSplitCatalog`: read-only query/snapshot.
- NDNSF-DI planning coordinator: ACK sanitization, decision invocation, validation,
  materialization, DI plan sealing, projection into generic collaboration
  roles/dependencies/assignments, and lifecycle evidence.
- NDNSF-DI Provider preparation context: exact Selection-bound assignment,
  provider-local bounded asynchronous preparation, and signed status.
- Base NDNSF Core: existing generic distributed-collaboration invocation and
  preplanned compatibility path, new generic deferred ACK-closure/plan-commit
  state, service messages, identity/security, ProviderToken, opaque optional
  lease, pending-request deadline, replay/idempotency, Selection,
  `CollaborationContext`, collaboration data/large objects, opaque status, and
  Response carriage. It does not parse or decide
  model, GPU, role, artifact, preparation, tensor, DAG, result, or DI recovery
  fields.
- NDNSF-DI dependency executor: atomically joins Selection, fresh local preparation, and
  verified direct-input latches for each role; no all-role scan and no
  model-specific communication branches.

| Concern | Base NDNSF | NDNSF-DistributedInference |
|---|---|---|
| Generic distributed-collaboration invocation, Request/ACK closure, one-shot plan commit, Selection, CollaborationContext, collaboration data/status/Response, names, signatures, NAC-ABE, permissions, one-time tokens | Owns | Uses and supplies opaque DI payloads |
| Preplanned roles/dependencies API | Owns as compatibility mode through the same state machine | Uses only for explicitly fixed/deployed compatibility paths |
| Opaque payload bytes, generic expiry/replay/idempotency, optional opaque lease/status handle | Owns | Defines profile and validates semantics |
| Model/split/placement/GPU admission/role tuple/artifact preparation | Must not interpret | Owns |
| Task input/options/result semantics and mutable inference state, including KV | Must not interpret | Owns through adapters and Provider state manager |
| Input/output object manifests, tensor DAG, ResultContract, compensation/replan | Must not interpret | Owns |
| Provider Selection transaction | Owns generic WAL, token/opaque-lease disposition, encrypted opaque commit blob/digest, and status carriage | Defines blob/acceptance semantics and idempotent runtime projection |

## Project Structure

### Documentation

```text
specs/163-di-collaboration-planning/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── contracts/
    ├── public-planning-api.md
    ├── model-adapter-state-cache.md
    ├── presplit-catalog-api.md
    ├── preparation-selection-flow.md
    ├── core-opaque-selection-transaction.md
    └── lifecycle-security-contract.md
```

### Expected source ownership

```text
NDNSF-DistributedInference/ndnsf_distributed_inference/
├── sdk/
│   ├── placement.py              # public pure strategy contract
│   ├── loader.py                 # allowlisted external implementation loading
│   └── executor.py               # existing bounded policy execution
├── planner/
│   ├── presplit_first.py         # built-in default
│   └── compatibility.py          # legacy ten-policy adapter, non-authoritative
├── app_sdk/
│   ├── placement.py              # snapshot, validation, plan assembly
│   ├── presplit.py               # catalog and operator mutation ports
│   ├── client.py                 # canonical lifecycle integration
│   └── contracts.py              # public high-level value types
├── adapters/
│   └── onnx/graph.py             # current graph/candidate evidence reused
├── splitter.py                   # common splitter output migration
├── provider.py                   # Selection-bound preparation and role state
├── plan.py                       # sealed collaboration-plan integration
└── core/
    ├── contracts.py              # DI envelopes, object/result identities
    ├── deployment_control.py     # DI offer/admission/Selection acceptance
    ├── recovery.py               # attempt, cancel, adoption, compensation
    └── execution.py              # DI dependency-driven DAG gate

ndn-service-framework/
├── ServiceUser.hpp/.cpp          # generic opaque payload/receipt/retry seam
├── ServiceProvider.hpp/.cpp      # token, opaque lease, deadline/idempotency
├── GenericSelectionTxnStore.hpp/.cpp
│                                    # generic encrypted WAL + participant seam
└── NDNSFMessages.hpp/.cpp        # generic opaque fields only

pythonWrapper/ndnsf/
└── service.py                    # generic participant registration only

tests/python/
├── test_ndnsf_di_placement_strategy.py
├── test_ndnsf_di_presplit_catalog.py
├── test_ndnsf_opaque_selection_lifecycle.py
├── test_ndnsf_di_selection_dataflow.py
├── test_ndnsf_di_dependency_dag.py
└── existing security/core/application tests

tests/unit-tests/
└── opaque-selection-lifecycle.t.cpp

Experiments/
└── NDNSF_DI_PlacementPreparation_Minindn.py
```

**Structure Decision**: Model-specific graph and placement concepts stay in
NDNSF-DI. GPU, role, artifact preparation, tensor dataflow, result, and recovery
also stay in NDNSF-DI. Base-Core changes are limited to reusable opaque
lifecycle seams that contain no DI semantics. Existing DI value objects
such as execution plans, dependencies, provider assignments, model fragment
keys, graph summaries, bounded policy execution, and decision validation are
adapted instead of duplicated.

## Implementation phases

### Phase 1 - Canonical collaboration carrier and planning contract

Deliver one source-of-truth value model for `ModelDescriptor`,
`SplitCandidate`, `ProviderPlanningView`, `PlacementRequest`, and
`PlacementDecision`.

- Inventory `ServiceUser::RequestCollaboration`, Python
  `request_collaboration[_async]`, `CollaborationPlan`,
  `collaborationAckRoleCoverageSatisfied`, Selection provider entries,
  `addCollaborationHandler`, and `CollaborationContext` callers/tests.
- Add one generic `DeferredCollaborationInvocation`/handle with
  `PREPLANNED` and `DEFERRED` modes. `begin_collaboration` publishes the normal
  Request and yields one immutable digest-bound ACK snapshot; `commit_plan`
  accepts generic roles, dependencies, key scopes, Provider assignments,
  artifact names, and opaque recipient payloads exactly once after
  `ACK_CLOSED`. It must be asynchronous with respect to the Face event loop.
- Project the current roles/dependencies-before-Request API into
  `PREPLANNED` on the same state machine; do not clone Selection, token,
  collaboration-data, status, timeout, or Response behavior.
- Inventory every existing DI-specific type/branch under
  `ndn-service-framework/` and `pythonWrapper/ndnsf/`; quarantine the frozen V1
  path, remove it from new V2 authority, and add a static ownership check plus a
  non-DI Core fixture. Migration must preserve generic Request/ACK/Selection/
  Response, permissions, tokens, opaque lease/status, and old evidence.
- Add `ModelPlacementStrategy` and the bounded external loader contract.
- Treat loaded strategies as allowlisted, digest-pinned, operator-trusted local
  code. The in-process timeout bounds normal operation but is not a sandbox or
  hard preemption boundary; strategy output remains untrusted until validated.
- Adapt existing split-plan, NDNSF-DI execution-plan, provider-assignment,
  model-fragment, ONNX graph, and validation values.
- Add the composed `ModelFamilyAdapter`, `InferenceTaskDescriptor`,
  `ApplicationInput`, and `InferenceStateContract` values. Prove an LLM
  adapter, object-detection adapter, and opaque one-node container adapter use
  the same carrier with no LLM fields in Core.
- Collapse the authoritative partition-plus-provider path into one joint
  decision.
- Retain the current ten-policy suite only through a named compatibility
  adapter; deprecate manual `APPClient.decide(requests)` as the canonical path.
- Prove pure execution and sensitive-field rejection before lifecycle
  integration.

Acceptance gate: a non-DI deferred fixture opens one collaboration, receives
one immutable ACK closure, commits once, and uses existing Selection,
`CollaborationContext`, data/status, and Response; early/conflicting/late
commits fail. The current preplanned API still passes through the same state
machine. An external DI strategy then changes both split and role assignment
without another Core protocol, and all malformed results fail before side
effects.

The phase also classifies the current deployment-oriented
`InferenceApplication.request(deployment=...)` surface as legacy
implementation reality until the model/task/input target API is wired;
documentation must not claim that the target surface is already implemented.

### Phase 2 - Pre-split catalog and default strategy

- Implement immutable content-addressed `PreSplitManifest`,
  read-only `PreSplitCatalog`, and independently authorized `PreSplitOps`.
- Import current `SplitterOutput` and model-specific artifacts into exact
  manifests without claiming a universal splitter.
- Implement `PreSplitFirstStrategy` with hard-constraint filtering, typed
  residency/availability evidence, bounded cost ranking, and deterministic
  tie-breaking.
- Allow a validated `StateReuseBinding` to affect cost only after exact
  adapter-owned identity, security-domain, epoch, expiry, layer coverage,
  resource, and pin feasibility checks. Model-shard reuse and derived-state
  reuse remain separate evidence classes.
- Prefer exact fresh GPU residency with pin/reload safety, then RAM, disk,
  repository-only, and new materialization.
- When no feasible manifest exists, derive a graph-valid, capacity-safe
  `SplitSpecification` by default through the configured named splitter. Use
  weights, activations, KV/workspace, transient overhead, and safety margin;
  never serialized bytes alone.
- Materialize only the chosen specification and publish a signed,
  content-addressed manifest through NDNSF-DistributedRepo before Selection.

Acceptance gate: feasible pre-split cases choose the expected exact manifest
and safe residency tier; feasible no-pre-split cases produce and publish one
deterministic safe manifest; stale, mismatched, incomplete, retired, revoked,
unbounded, corrupt, or partially published cases are rejected before Selection.

### Phase 2b - Provider-local inference-state reuse

- Implement `InferenceStateContract` classification and default terminal
  destruction for every undeclared mutable state.
- Add `EXACT_PREFIX_KV_V1` with opaque key digest and exact model, semantics,
  adapter/runner, split/layer, prefix-token, position, precision, layout,
  security-domain, boot/cache/entry epoch, expiry, and byte binding.
- Advertise only bounded sanitized evidence in `DIProviderOfferV2`; seal the
  accepted `StateReuseBinding` into `commit_plan` and final Selection.
- Atomically install state authorization and pin/reservation with Provider
  Selection acceptance, then revalidate immediately before use.
- Keep baseline state local. Optional encrypted migration has a separate
  adapter contract and is not required for Spec 163 completion.
- Destroy request-scoped state and release grants/pins at terminal convergence;
  retain reusable state only under explicit authorization, TTL, and budget.

Acceptance gate: cold, request-cleanup, exact local hit, every identity,
security-domain, epoch, expiry, eviction, restart, failed-pin, and clean
fallback case matches the frozen reference. No prompt/token disclosure,
cross-tenant reuse, approximate reuse, or public model-repository publication
occurs.

### Phase 3 - ACK offer, Selection, and asynchronous preparation lifecycle

- Route the canonical application call through NDNSF
  `begin_collaboration(mode=DEFERRED)`; move plan choice after the immutable
  capability `ACK_CLOSED` snapshot.
- Permit only local task/input/options validation before Request publication.
  Model graph inspection, dependency analysis, candidate enumeration, and any
  split materialization/publication are post-`ACK_CLOSED` operations and must
  use the exact Provider offer snapshot. With no valid positive ACK, fail before
  doing model work or publishing model bytes.
- Build typed sanitized provider/network/catalog snapshots.
- Validate and seal the selected DI collaboration plan.
- Materialize only a selected generated candidate through a trusted port.
- After materialization, call generic `commit_plan` exactly once on the same
  invocation. Project only roles, dependencies, key scopes, Provider
  assignments, artifact names, and opaque DI assignment bytes. Let existing
  NDNSF Selection delivery and `CollaborationContext` carry execution.
- Keep the original inference Request pending under one negotiated hard
  deadline rather than publishing a preparation Request.
- Define a signed `DIProviderOfferV2` inside the opaque generic ACK payload,
  bound to schema/canonical-encoding/acceptance-predicate versions plus
  requester/service/invocation/request/attempt/model-intent/boot-epoch/
  resource-sequence/deadline/expiry.
- Add bounded exact disk/RAM/GPU/repository residency entries with
  model/manifest/artifact/range/backend/precision/runtime/trust/device,
  boot/cache epoch, freshness, pin/lease, eviction, and reload facts.
- Exclusively offer a GPU-RAM budget in an NDNSF-DI admission ledger at ACK
  time; sanitize it for strategy use while keeping the generic ProviderToken
  and opaque lease proof private. Concurrent requests cannot receive
  overlapping DI promises. Base NDNSF does not interpret GPU units.
- Validate that each assignment is backed by a fresh, validated
  `DIProviderOfferV2` carried by a positive generic ACK and that the aggregate
  estimated peak GPU RAM assigned to a Provider does not exceed the offer's
  reserved budget.
- Create exactly one immutable logical positive final-Selection identity per
  selected Provider carrying its complete recipient-specific role tuple;
  permit only byte-identical bounded retransmissions, bind the ACK offer digest,
  and use the Core-owned generic `GenericSelectionTxnStore` WAL from
  `contracts/core-opaque-selection-transaction.md`. A pure registered
  NDNSF-DI participant returns one canonical encrypted opaque commit blob and
  acceptance payload; one Core WAL fsync atomically consumes the ProviderToken,
  commits the opaque lease disposition, and makes that blob authoritative.
  NDNSF-DI then idempotently projects the DI admission ledger, exact role tuple,
  grants, and generation from the record. Start asynchronous
  verify/fetch/load/warm only after `COMMITTED`. Same-attempt incremental role
  additions are invalid.
- Persist the requester retransmit/abort journal encrypted at rest and bounded
  by the attempt deadline. Treat missing acceptance evidence as `UNKNOWN`;
  retransmit the identical Selection or query status without inventing a
  cross-Provider acceptance cover.
- Bind operation status and `RoleLocalReadyReceipt` to the final Selection
  digest, not to a pre-Selection control message.
- Bind every recipient-encrypted per-role/per-input key grant into the same
  Selection digest and reject tamper or cross-request/attempt/Provider/role
  replay; no earlier message grants input access.
- Send exact `NOT_SELECTED` or release for unused validated DI offers carried
  by positive generic ACKs.
- Treat per-Provider Selection delivery and role dataflow as orthogonal: never
  wait for complete Selection delivery or all selected models to become locally
  ready before another already selected role may prepare or execute.
- Replace the fixed pending cleanup with one absolute,
  generation-fenced/cancellable monotonic budget derived from signed wire
  deadlines and bounded clock-skew rules; duplicate messages and status
  observations cannot extend it.

Acceptance gate: a missing, negative, stale, wrong-boot, expired, replayed, or
insufficient-GPU-RAM ACK, or an out-of-envelope assignment, prevents
unauthorized Selection and releases bounded resources. A valid in-envelope
Selection is locally linearized even when the model is not locally ready.
Evidence shows one generic `DEFERRED` collaboration invocation, one Request,
one immutable ACK closure, one accepted plan commit, direct existing final
Selection per selected Provider, one Core WAL `COMMITTED` linearization, one ProviderToken
consumption, optional opaque-lease disposition, one encrypted opaque DI commit
blob, idempotent multi-role projection, and mandatory acceptance record,
`UNKNOWN`/retry recovery, no cross-request GPU-budget double commitment, and
asynchronous local preparation with no `PreparationCommit` traffic or global
acceptance barrier.

### Phase 4 - Event-driven role execution and compensation

- Connect the sealed plan to the existing dependency-driven executor.
- Represent Selection authorization, local model readiness, and each direct
  input edge as independent role-local latches.
- Re-evaluate only the affected role after Selection validation, local
  preparation completion, authenticated input arrival, cancellation, failure,
  or deadline expiry.
- Start a role atomically at most once when all three readiness classes are
  satisfied. This is at-most-once admission per role generation, not proof that
  physical GPU computation happens exactly once. Retain an early input until
  local preparation completes and retain local readiness while waiting for
  input.
- Fence the `WAITING -> RUNNING` transition by request, attempt, plan digest,
  role, provider boot epoch, and execution generation. Terminal
  `FAILED`/`CANCELLED`/`EXPIRED` or superseded-attempt tombstones win every race
  against local-ready, input, output, and status events.
- Reject late old-attempt events before they can restore eligibility, wake a
  direct consumer, or contribute to a terminal Response.
- Require every inter-role object to carry a complete
  `InputOutputObjectManifest`; ensure chain, fan-in, and fan-out outputs wake
  direct consumers without waiting for unrelated or downstream roles.
- Seal exactly one `ResultContract` per plan. Multiple sinks require an
  explicit deterministic DI aggregator or complete aggregation rule. Admit at
  most one terminal `DIResultEnvelopeV2`, then carry it in one generic Response.
- Remove model/demo-specific communication control from the accepted path.
- Add bounded compensation using verified dependency-closure evidence under the
  original model semantics and deadline. Cross-attempt reuse requires explicit
  `AdoptedInputEvidence`; lineage similarity alone is insufficient.
- Implement cancellation as local generation-fenced terminal CAS plus
  authenticated eventual convergence and idempotent release. Do not claim
  distributed atomic cancellation under partitions.

Acceptance gate: one chain and one real fan-in/fan-out workload use the same
executor and match frozen local reference outputs. Stage 0 executes while a
downstream role is still preparing; input-before-model and model-before-input
both start the downstream role exactly once when the second condition arrives;
cancel-versus-last-input, failure-versus-local-ready,
deadline-versus-execution-start, and old-output-after-replan races start zero
unauthorized work; cancel-versus-Response accepts only the first local terminal
CAS; partial terminal output is never accepted.

### Phase 5 - Security and network closure

- Run focused unit, contract, negative, application API, token, replay,
  permission, object-manifest, result, journal, and artifact validation.
- Run a sequential reference model with declared bounds, exhaustive finite
  transition/WAL/crash-cut enumeration where tractable, and generated
  duplicate/reorder/loss/retry/partition/concurrency schedules with retained
  seeds and replayable shrunk counterexamples. Report zero observed violations
  in that declared corpus for at-most-once admission, one accepted terminal
  result, capacity exclusion, authority fencing, and bounded cleanup; do not
  call property testing a general proof.
- Run the static Core-ownership gate and a non-DI service fixture proving that
  the generic runtime neither imports nor parses model/GPU/role/artifact/tensor
  fields.
- Run the local Docker fake-payload closed loop within the 4 GiB/5 GiB
  memory/swap boundary.
- Run a frozen MiniNDN matrix using a `Qwen/Qwen3-0.6B` `ModelRef` with exact
  model-manifest content and semantics digests for default/custom planning,
  cold dynamic split and repository
  publication, exact warm cache reuse, fan-in/fan-out,
  missing/stale/insufficient-GPU-RAM ACK, out-of-envelope Selection,
  concurrent GPU-budget contention, one-Provider/multi-role Selection,
  selected-but-preparing, partial Selection delivery with upstream progress,
  input-before-model,
  model-before-input, post-Selection role failure, compensation, tamper, replay,
  stale boot epoch, expiry, malicious signed-but-semantically-wrong output
  handling, noncanonical encoding, object substitution, downgrade, and
  mixed-version rejection.
- Use five real prompts, at most 64 generated tokens, one unmeasured warmup,
  and five measured repetitions per prompt. Retain complete answers, reference
  consistency, TTFT, per-token latency, total latency, tokens/s, all preparation
  phase timings, cache tier/reason, bytes fetched, and distributions.
- When local CUDA is unavailable, retain network/correctness/host-cache
  evidence but mark GPU residency/reload criteria deferred; do not simulate a
  GPU result.
- Update maintained English/Chinese API and operator documentation together.
- Audit source/docs against this plan and the dissertation scope.

Acceptance gate: no audit `BLOCK`, lifecycle/security invariants pass under the
history oracle, ownership checks pass, all required regressions pass, negative
rows are retained, and no unsupported malicious-computation, deadlock-freedom,
distributed-atomicity, performance, or universal-optimizer claim is made.

### Phase 6 - Optional TigerCluster/large-model requalification, outside this feature

Only after Phase 5 and a fresh explicit authorization:

- reactivate Spec 162 explicitly;
- add a fresh requalification task rather than rewriting completed historical
  tasks;
- create new source/OCI/SIF identities and evidence schema;
- choose and pin the separately authorized large Qwen revision;
- allow the default strategy to reuse or generate its split rather than
  requiring three pre-registered stage artifacts;
- confirm `PreSplitFirstStrategy` chose them from real ACK state;
- obtain one fresh explicit live authorization before any job.

## Migration and rollback

- Spec 129 remains frozen evidence for `SELECTION_LOADS_V1`.
- Existing `request_collaboration` callers that provide roles/dependencies
  before Request are classified as `PREPLANNED` and continue through the same
  generic invocation/Selection state machine. The Spec 163 application path is
`DEFERRED`; static and runtime gates reject use of the preplanned entrypoint
  as its default.
- Rollout first adds the generic deferred handle and non-DI fixture, then
  migrates NDNSF-DI fixed-plan callers, and only then removes any duplicate DI
  orchestration lifecycle. Rollback disables `DEFERRED` while preserving
  `PREPLANNED`; no persisted deferred invocation is reinterpreted as preplanned.
- New deployments opt into a versioned NDNSF-DI payload profile carried in
  generic messages; one attempt never mixes profiles. Unknown schema,
  canonical-encoding, capability, or acceptance-predicate versions fail closed
  rather than downgrade. This profile makes a validated DI offer binding and
  makes final Selection the sole concrete assignment transition without adding
  a model-aware base-NDNSF mode.
- `SelectionGatedProvider.select()` is not silently redefined. The new path
  receives explicit ACK-offer validation and asynchronous Selection-dataflow
  seams; the old behavior remains behind a named compatibility adapter until
  migration closes.
- One request retains one UserToken and one existing ProviderToken per positive
  Provider ACK. Exactly one positive final Selection per selected Provider
  consumes that token and atomically installs its complete role tuple. Pending
  state is bounded by the negotiated hard deadline and cannot be extended by
  duplicate activity.
- Rollback disables the DI V2 profile only for explicitly V1-compatible
  deployments; no silent fallback is permitted. V2 evidence and role-local
  resources remain inspectable and expire or release safely.
- Existing DI-specific codecs and handlers in base NDNSF are frozen migration
  debt: move their authority under `NDNSF-DistributedInference`, retain only
  generic opaque seams in Core, and block future DI field additions with a
  static ownership test.
- The current token-before-DI-validation path cannot serve V2. Migration first
  introduces the generic WAL/participant seam and routes token/lease decisions
  through it; only then may NDNSF-DI register its V2 participant. Rollback
  drains or expires WAL records and never reinterprets their blobs through V1.
- Existing Qwen jobs, terminal states, and artifacts are never relabeled or
  deleted by this migration.

## Evidence and claim boundaries

Every accepted decision records:

```text
strategy name/version/state digest
validated provider/network/catalog snapshot digests
split candidate or pre-split manifest digest
placement decision and sealed collaboration-plan digests
public invocation identity, attempt identity, and single ACK-closure identity
generic collaboration mode, invocation-handle identity, immutable
ACK-closed digest, plan-commit digest/state, and commit retry/conflict history
positive-ACK offer, GPU-RAM budget, and optional reservation digests
provider boot epochs and role-local-ready receipt digests
per-Provider final Selection identity, complete role-tuple digest, and
ProviderToken proof hash
mandatory Selection-acceptance record, transaction sequence, and UNKNOWN/retry
history
generic WAL record/digest, registered participant identity/version, encrypted
opaque commit-blob digest, and idempotent projection outcome
per-role/per-input key-grant digests and terminal access state
input/output object manifests, ResultContract, accepted DI result, and generic
Response identity
request/ACK counts, unique Selection-decision count, Selection
transmission/retry count, and phase timestamps
per-Provider offer/Selection-delivery state plus per-role selected, local-ready,
inputs-ready,
execution-started, output-published, and terminal timestamps
dependency/operation-status evidence proving no all-role-ready gate
cancel/expiry/Response terminal-CAS outcome, compensation, and any
AdoptedInputEvidence
```

Passing this feature proves that NDNSF-DI supplies an automatic, extensible
collaboration-planning and execution foundation. It does not prove globally
optimal partitioning, every model family, production scheduling, or Qwen
performance. Signatures prove authenticated origin and integrity, not correct
computation by a malicious Provider. Liveness is conditional on stated
deadline, retry, storage, and eventual-delivery assumptions; no distributed
atomicity or deadlock-freedom theorem is claimed.

## Complexity Tracking

No constitution violation is accepted. The single-call design requires a
bounded long-lived pending request, binding positive-ACK resource offer, exact
Selection assignment, and request/role-bound status, but avoids a second
Request, `PreparationCommit`, complete-system readiness barrier, fifth generic
service-message kind, cross-provider atomic commit, and token reuse.
