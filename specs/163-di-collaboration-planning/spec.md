# Feature Specification: Pluggable Distributed-Inference Collaboration Planning

**Feature Branch**: `[163-di-collaboration-planning]`

**Created**: 2026-07-28

**Status**: Draft — proposed V2 design authority; live model experiments paused

**Input**: User description: "Stop the Qwen experiment first. Separate model
partitioning and provider-role assignment into an externally implementable
strategy, provide a built-in pre-split-first strategy and pre-split API, and
interpret a generic `ACK=true` carrying a validated `DIProviderOfferV2` as the
Provider's willingness to accept any compatible role set that fits the offer's
advertised capability and aggregate GPU-memory envelope. A bare generic ACK
does not acquire DI semantics. Prefer an exact reusable shard already resident
on a Provider GPU; otherwise derive a split after ACK closure, publish its
immutable artifacts through NDNSF-DistributedRepo, and let selected Providers
fetch, verify, cache, and load their assigned shards.
One final Selection per selected Provider directly assigns that Provider's
complete role tuple; role-local preparation and data-driven execution
remain inside the same inference request without an all-role readiness barrier.
Use NDNSF's existing generic distributed-collaboration API as the canonical
carrier. Extend that API with a generic deferred-plan lifecycle
`begin_collaboration -> ACK_CLOSED -> commit_plan`; retain its current
roles/dependencies-before-Request form only as a preplanned compatibility
path, not as the default dynamic-planning path. Make the application API
model-family-neutral through composed adapters, and support secure reusable
inference state, including exact prefix KV cache as an LLM adapter profile,
without adding LLM semantics to NDNSF Core."

## Canonical Request Lifecycle

```text
InferenceApplication.request
  -> resolve ModelFamilyAdapter + task/input/state contracts
  -> NDNSF begin_collaboration
  -> Request
  -> ACK collection
       ACK=true + validated DIProviderOfferV2:
         capability-bounded willingness + fresh disk/RAM/GPU shard residency
         + bounded opaque reusable-state evidence
  -> ACK_CLOSED
  -> split and provider-role planning
       prefer an exact feasible reusable split;
       optionally bind exact authorized derived-state reuse;
       otherwise derive a capacity-safe SplitSpecification
  -> if needed, trusted split materialization and immutable
       NDNSF-DistributedRepo publication
  -> NDNSF commit_plan on the same collaboration invocation
       one final Selection per selected Provider assigns its complete role tuple
  -> data-driven distributed execution
       per role: revalidate exact GPU reuse, or asynchronously
                 fetch by NDN, verify, persist, and load
                 + revalidate any selected local derived-state entry
                 + valid final Selection
                 + verified direct inputs
                   -> atomically admit at most once per generation
  -> Response
  -> adapter-governed state release or bounded authorized retention
```

`begin_collaboration`, `ACK_CLOSED`, and `commit_plan` are generic local API
and durable-state boundaries in NDNSF's existing collaboration invocation;
they are not new DI wire message kinds. NDNSF-DI owns the asynchronous
post-ACK planner, materializer, and opaque assignment payloads. NDNSF owns the
generic collaboration handle, closed ACK snapshot, one-shot plan commit,
existing Selection delivery, collaboration object transport, status seam, and
Response carriage.

“Distributed execution” is not one globally synchronized phase. Local model
preparation, predecessor-output arrival, and role execution overlap. A selected
role starts as soon as its own local preparation and all direct inputs are
ready; it never waits for unrelated or downstream roles. There is no
`PreparationCommit`, commit receipt, or second role-acceptance round.

The generic lifecycle does not assume text generation. A registered
`ModelFamilyAdapter` composes graph/split, task I/O, state, and runner ports.
An adapter that cannot expose an internal graph may emit one opaque atomic graph
node and therefore remains usable without pretending that a safe split exists.
KV cache is one `EXACT_PREFIX_REUSABLE` specialization of the general
`InferenceStateContract`; vision, speech, diffusion, container, and future
model families use the same application and collaboration carrier without
LLM-specific fields in NDNSF Core.

Throughout this specification, “one final Selection per selected Provider”
means one immutable logical decision identity/digest, one role-tuple
installation, and one ProviderToken consumption. The same authenticated wire
object may be retransmitted idempotently within the deadline, and transmission
and retry counts are recorded separately.

One public inference invocation may contain a bounded sequence of wire attempts.
Each attempt has at most one Request; an attempt that reaches Request
publication emits exactly one, and an attempt that reaches ACK closure closes
exactly one candidate set. Replanning that changes an assignment starts a fresh
attempt with fresh ACKs, ProviderTokens, leases, plan, and Selection identities;
the public request handle and original total deadline remain unchanged. The
protocol guarantees at-most-once execution admission per
role generation and exactly one accepted terminal result for the public
invocation. It does not claim physical exactly-once computation across process
crashes.

The normative ownership, trust, state-machine, failure, object-security,
recovery, and liveness rules are in
`contracts/lifecycle-security-contract.md`. In particular:

- NDNSF Core authenticates and transports opaque payload bytes and owns generic
  collaboration invocation/ACK-closure/plan-commit state, tokens, leases,
  deadlines, replay, Selection delivery, collaboration data, and status
  transport;
- NDNSF-DI owns the DI payload schema and all model, GPU, role, artifact,
  preparation, tensor, DAG, result, and compensation meaning;
- Selection acceptance has one mandatory authenticated per-Provider
  linearization record, but no cross-Provider readiness cover;
- cancellation is coordinator-local linearization plus bounded remote
  convergence, not distributed atomic cancellation;
- safety is unconditional under the stated cryptographic assumptions, while
  successful completion is conditional on bounded delivery and Provider
  availability.

## User Scenarios & Testing

### User Story 1 - Generate one collaboration plan automatically (Priority: P1)

As an application developer, I want to request inference without manually
writing provider roles, artifact messages, or tensor-transfer steps so that the
distributed-inference layer can turn model information and current provider
offers into one reviewable collaboration plan.

**Why this priority**: Automatic collaboration-plan generation is the central
NDNSF-DI work proposed in the dissertation and is the prerequisite for
meaningful distributed-model experiments.

**Independent Test**: Given a frozen model description, dependency graph,
request objective, and a set of validated provider ACK snapshots, planning
either returns one complete, internally consistent plan or a structured
no-feasible-plan result. The caller does not construct low-level NDNSF messages.

**Acceptance Scenarios**:

1. **Given** a model with feasible split candidates and enough provider
   capabilities, **When** inference planning completes, **Then** the result
   identifies the chosen split, every provider role, all model/runtime
   artifacts, tensor dependencies, execution order, and fallback or
   compensation policy.
2. **Given** stale, unauthenticated, incompatible, or insufficient provider
   offers, **When** planning runs, **Then** those offers cannot appear in the
   accepted plan and an infeasible request fails without execution.
3. **Given** a non-ONNX model family, **When** a model-specific splitter emits
   the common splitter output, **Then** the same collaboration planner and
   executor contracts can consume it without model-specific wire behavior.

---

### User Story 2 - Replace the planning policy without changing NDNSF (Priority: P1)

As an external researcher, I want to implement my own model-placement strategy
against a stable public contract so that I can experiment with partition and
provider-assignment algorithms without modifying NDNSF security, transport, or
execution code.

**Why this priority**: The dissertation must provide reusable framework support
while leaving room for other researchers to implement more advanced algorithms.

**Independent Test**: A test strategy changes both the selected split and
provider-role assignment using only documented inputs and outputs. It has no
access to network publication, tokens, artifact mutation, GPU loading, or final
Selection authority.

**Acceptance Scenarios**:

1. **Given** a third-party strategy implementation, **When** it is registered
   for one request, **Then** its valid decision is used without edits to the
   NDNSF runtime or model-specific provider applications.
2. **Given** the ACK set is closed, **When** the strategy is invoked, **Then**
   each candidate Provider view exposes validated usable capacity, queue/wait
   state, RTT, bandwidth, and exact GPU/RAM/disk/repository shard residency,
   and the strategy jointly chooses the split and role assignment from those
   views rather than from a pre-request deployment.
3. **Given** a strategy that times out, raises an error, returns an unknown
   provider, exceeds its candidate budget, or returns an invalid graph,
   **When** the result is validated, **Then** the request fails closed before
   any preparation side effect or Selection.
4. **Given** identical canonical inputs and strategy state, **When** planning is
   repeated, **Then** the plan identity and decision evidence are reproducible.
5. **Given** the dynamic-planning profile, **When** the application makes one
   inference call, **Then** NDNSF-DI opens one generic NDNSF collaboration
   invocation, waits for its immutable `ACK_CLOSED` snapshot, commits the
   validated plan exactly once through that same invocation, and creates no
   parallel DI Request/ACK/Selection protocol.

---

### User Story 3 - Reuse exact shards or split after ACK closure (Priority: P2)

As an NDNSF-DI operator, I want to register immutable pre-split model plans and
have the default strategy prefer a feasible exact registered plan, especially
one already GPU-resident, but automatically derive and publish a new split when
no feasible entry exists. Normal repeated inference therefore avoids
unnecessary split, transfer, and load work without making pre-splitting a
prerequisite.

**Why this priority**: Pre-split-first preserves the current practical
deployment path while changing its ownership from hard-coded application logic
to an explicit policy decision.

**Independent Test**: With one exact, feasible pre-split entry and several
dynamic choices, the default policy selects the pre-split entry and ranks an
exact GPU-resident placement first. With no feasible entry, it derives a
capacity-safe `SplitSpecification`, after which the trusted coordinator
materializes and publishes the immutable manifest before Selection. An entry
with the wrong model name/content/semantics digest, graph semantics, artifact digest, backend,
precision, trust policy, Provider boot epoch, or freshness is never reused.

**Acceptance Scenarios**:

1. **Given** a feasible pre-split entry matching the exact model name,
   content digest, semantics digest, and
   request semantics, **When** the default policy runs, **Then** it selects that
   entry, prefers exact fresh GPU residency over RAM, disk, or repository-only
   residency, and records the ranking evidence.
2. **Given** no feasible pre-split entry, **When** the default strategy runs,
   **Then** it derives a model-specific split from the closed ACK snapshot,
   Provider capacity envelopes, graph constraints, and runtime-peak estimates;
   the trusted coordinator publishes the result through
   NDNSF-DistributedRepo before final Selection.
3. **Given** the model adapter cannot produce a safe runtime-size bound or no
   assignment fits after safety margins, **When** planning runs, **Then** it
   returns a structured no-feasible-plan result before materialization or
   Selection rather than guessing from serialized model bytes.
4. **Given** a completed first request leaves immutable model/runtime shards in
   an allowed Provider cache, **When** a later exact-compatible request is
   planned, **Then** the Provider advertises fresh tiered residency and the
   strategy can reuse it without retaining request inputs, outputs, activations,
   plaintext grants, or ordinary request-scoped state.
5. **Given** an LLM adapter declares `EXACT_PREFIX_REUSABLE` state and a
   Provider holds an authorized exact-compatible prefix-KV entry for its own
   layer range, **When** a later request binds the same opaque state-key digest,
   security domain, model/semantics/adapter/runner/split/layout identity and
   prefix-token digest, **Then** planning may bind that entry and avoid prefill
   for the covered prefix after Selection-time revalidation.
6. **Given** any state-key component, tenant/security domain, Provider boot or
   cache epoch, expiry, layer range, precision, position convention, or KV
   layout differs, **When** planning evaluates the entry, **Then** the entry is
   not reused and normal clean execution remains available.
7. **Given** an adapter declares only request-scoped mutable state, **When** the
   invocation reaches terminal convergence, **Then** the state is destroyed;
   it never becomes a cross-request cache merely because its bytes remain in
   device memory.

---

### User Story 4 - Assign roles and execute them as they become runnable (Priority: P2)

As a requester, I want a positive generic ACK carrying a validated signed
`DIProviderOfferV2` to mean that a Provider is willing to accept any compatible
role set within its advertised capability and aggregate GPU-memory envelope, so
that one final Selection can directly assign all roles allocated to that
Provider and a slow downstream model does not prevent an already runnable
upstream stage from making progress.

**Why this priority**: This preserves final Selection as execution authority
while removing the coordinator-level all-model-ready barrier. It also allows
model preparation, input transfer, and execution to overlap along the selected
dependency graph.

**Independent Test**: One secured inference request collects positive,
authenticated ACK offers with sufficient GPU-memory envelopes, seals one plan,
and uses one final Selection per selected Provider to atomically assign its
complete role tuple without a
`PreparationCommit` round. Each selected Provider starts request-scoped local
preparation asynchronously, reusing eligible prewarm/cache work, and each role
is admitted to execution at most once in that generation when and only when
its own model is ready and all authenticated direct inputs are available.
The test covers model-ready-before-input and input-before-model-ready.

**Acceptance Scenarios**:

1. **Given** each planned assignment fits a fresh validated DI offer carried by
   a positive generic ACK, **When** the requester
   issues final Selection, **Then** one Selection per selected Provider binds
   the exact ACK snapshot, plan, Provider boot epoch, complete role tuple,
   artifacts, resources, and deadline, consumes that Provider's existing
   one-time ProviderToken once, and does not require any role to be locally
   ready.
2. **Given** Stage 0 is selected, its local model and direct request input are
   ready, and Stage 1 is still preparing, **When** Stage 0's execution gate is
   evaluated, **Then** Stage 0 executes without waiting for Stage 1.
3. **Given** a downstream stage has received all predecessor data but its local
   model is still preparing, or its model is ready but predecessor data has
   not arrived, **When** the missing condition later becomes true, **Then** the
   stage starts automatically without a new coordinator activation message.
4. **Given** an ACK is stale, its Provider restarted, or a planned assignment
   exceeds its advertised GPU-memory envelope, **When** the requester validates
   the plan, **Then** it does not issue positive Selection for that invalid
   assignment. A preparation failure after valid Selection is handled by the
   plan's bounded dependency-closure compensation or abort policy and cannot
   produce a partial terminal Response.
5. **Given** a duplicate final Selection with the same identity and digest,
   **When** it is received, **Then** handling is idempotent; a conflicting
   Selection for the same attempt is rejected as replay or equivocation.
6. **Given** one Provider is assigned two compatible roles, **When** final
   Selection is delivered, **Then** both roles are installed atomically in one
   complete tuple using one ProviderToken; a missing role or later incremental
   role addition is rejected.
7. **Given** Stage 0's Provider has accepted Selection while a downstream
   Provider's Selection is still being retried, **When** Stage 0 becomes
   locally ready and has verified request input, **Then** it executes without
   waiting for complete Selection delivery; permanent downstream delivery
   failure cancels or replans the affected closure and yields no partial
   terminal Response.
8. **Given** an application caller submits inference, **When** cold
   role-local preparation is required, **Then** the same request handle reports
   ACK, planning, Selection, preparation, input-waiting, execution, and final
   result; the caller never invokes a second preparation or inference request.
9. **Given** Selection reached a Provider but its acceptance reply was lost,
   **When** the requester reconciles delivery, **Then** it treats the outcome as
   `UNKNOWN`, retransmits only the byte-identical Selection, and resolves the
   Provider through its authenticated acceptance record or local expiry without
   reusing the ProviderToken.
10. **Given** cancellation races with a valid terminal Response, **When** the
    request journal performs its terminal compare-and-set, **Then** the first
    durably accepted terminal decision wins and later remote events cannot
    reverse it.

---

### User Story 5 - Execute chain and fan-in/fan-out plans generically (Priority: P3)

As a model-adapter author, I want one dependency-driven executor to consume the
collaboration plan so that YOLO, a small fully connected model, an LLM pipeline,
and future model-specific splitters do not implement separate communication
protocols.

**Why this priority**: The dissertation proposes chain and fan-in/fan-out DAG
execution as evidence that the framework is not a model-specific demo.

**Independent Test**: One chain plan and one real fan-in/fan-out plan use the
same execution engine. Each role fetches all declared inputs, is admitted once
per generation,
publishes declared outputs by reference, and accepts the terminal result only
after all graph dependencies and evidence checks pass.

**Acceptance Scenarios**:

1. **Given** a role with two incoming tensor edges, **When** only one input is
   available, **Then** the role does not execute.
2. **Given** a role with valid final Selection whose local model is ready and
   all direct inputs are authenticated, **When** either the last input arrives
   or local preparation completes, **Then** the role becomes runnable and
   is admitted to execution at most once in that generation.
3. **Given** a role whose output feeds two downstream roles, **When** it
   completes, **Then** both consumers can fetch and independently verify the
   named output object.
4. **Given** a selected provider failure, **When** the plan defines a valid
   fallback or compensation action, **Then** the attempt follows that declared
   policy and never accepts a partial terminal result.

### Edge Cases

- A provider ACK is authentic but older than the planning freshness window.
- A provider restarts between ACK, Selection, local preparation, and execution.
- Cold preparation outlives the legacy pending-request cleanup interval but
  remains inside the negotiated total request deadline.
- Two requests concurrently reserve the same finite provider capacity.
- One plan assigns two compatible roles to one Provider and must consume only
  that Provider's one ACK/ProviderToken through one atomic Selection bundle.
- A pre-split catalog entry names an artifact that is missing, corrupt, or
  incompatible with the provider backend.
- A signed ACK claims an exact GPU-resident shard, but the claim expires, the
  Provider reboots, or an unpinned cache entry is evicted before Selection.
- A cached shard has the right layer range but a different model name,
  content digest, semantics digest,
  tokenizer, digest, backend, precision, runtime ABI, or trust policy.
- Reusable resident bytes are double-counted as both occupied capacity and new
  load demand, or transient workspace and safety headroom are omitted.
- A new request requires another model shard while the old shard is resident;
  selected and in-flight shards must not be evicted, while unpinned shards may
  be evicted only under a bounded, observable cache policy.
- A model-specific splitter produces overlapping chunks, missing graph outputs,
  cycles, ambiguous tensor producers, or unresolved dynamic tensor sizes.
- Planning is successful but artifact materialization fails before Selection.
- Materialization succeeds but repository publication is partial, a segment is
  corrupt, or the immutable manifest cannot be sealed before Selection.
- A planner assigns a role whose estimated peak or aggregate GPU RAM exceeds
  the Provider's positive-ACK envelope.
- Final Selection arrives while zero, one, or several required fragments are
  already cached or prewarmed; request-specific local-ready evidence is created
  only after Selection validates the assignment.
- An upstream output arrives before the downstream model is ready, or the
  downstream model becomes ready before its input arrives.
- A fan-in role receives required predecessor outputs in different orders.
- A positive generic ACK carrying a DI offer, ProviderToken, preparation
  status, or final Selection is replayed under another request, attempt, plan,
  Provider, role, or boot epoch.
- Cancellation races with the last required input, failure races with local
  readiness, or deadline expiry races with the once-only execution transition.
- A late local-ready receipt, input, output, or status from an old attempt
  arrives after compensation or replanning created a new attempt.
- Final Selection reaches only a strict subset of the planned Providers before
  delivery failure or deadline.
- A legacy peer supports the current Selection-then-load behavior but not the
  new preparation semantics.
- A fallback changes numerical semantics or model identity instead of merely
  changing an allowed placement.
- A coordinator restarts after journaling, sending, or remotely installing only
  a subset of Provider Selections.
- A Provider crashes between token validation, lease commit, role-tuple install,
  acceptance-record publication, role start, output publication, or Response.
- Selection is installed but its authenticated acceptance record is delayed or
  lost.
- A cancel message is partitioned while a non-preemptable GPU kernel continues;
  the resulting late output must remain unaccepted.
- A fan-out plan has multiple sinks but no explicit result-aggregation role.
- An authorized but faulty Provider signs an incorrect tensor: origin is proven
  but computational correctness is not, unless the application enables result
  verification, redundancy, or attestation.
- A capability downgrade, non-canonical encoding, boot-epoch reuse, oversized
  segmented object, mixed-segment object, or stale key grant is attempted.

## Requirements

### Functional Requirements

- **FR-001**: NDNSF-DI SHALL expose one canonical strategy extension point that
  jointly selects a model split and assigns providers to all required roles.
- **FR-002**: Strategy input SHALL include an immutable model description,
  an immutable common `ModelGraphSnapshot` produced by the registered
  model-specific adapter, candidate split information, request
  objective and constraints, and one sanitized `ProviderPlanningView` derived
  from each eligible ACK in the closed ACK set. Each view SHALL expose available
  backends, usable capacity, queue/wait state, RTT, bandwidth, exact
  GPU/RAM/disk/repository shard residency, source/freshness/confidence metadata,
  and immutable artifact locations without exposing tokens or mutable handles.
  Input SHALL also include runtime peak estimators and safety margins, a
  decision deadline, and a candidate budget.
  For ONNX, `ModelGraphSnapshot` SHALL expose canonical nodes, producer-consumer
  tensor edges, topological order, legal cut edges, model I/O contracts,
  dtype/shape, and bounded transfer-size estimates where known, and SHALL bind
  the exact ONNX graph digest.
- **FR-003**: Strategy output SHALL be data only and SHALL identify the selected
  exact model name/content/semantics identity and either an existing immutable
  manifest or a new
  `SplitSpecification`, complete role graph, provider assignment, model and
  runtime artifact requirements, intermediate object naming rules, predictable
  prefetch objects, execution order, estimated costs, fallback or compensation
  policy, and the exact input snapshot used. A strategy decision SHALL NOT
  itself materialize, publish, fetch, or load artifacts.
  A generated `SplitSpecification` SHALL cover every graph node exactly once,
  cut only adapter-declared legal edges, preserve the graph topology and model
  I/O semantics, and identify every cross-partition tensor dependency.
- **FR-004**: External strategies SHALL NOT receive authority to publish NDNSF
  messages, consume invocation tokens, mutate the artifact catalog, load
  devices, or issue final Selection.
- **FR-005**: NDNSF-DI SHALL validate every strategy result independently,
  including graph completeness, role coverage, provider eligibility, ACK
  freshness and boot epoch, model semantics, artifact identity, resource
  constraints, deadline, and decision-input binding.
- **FR-006**: Strategy timeout, exception, malformed output, budget violation,
  stale input, or validation failure SHALL terminate planning without
  preparation or execution side effects.
- **FR-007**: Model-specific splitters SHALL publish one common splitter output
  representing chunks, graph edges, required artifacts, runtime requirements,
  model semantics, and candidate costs; they SHALL NOT define model-specific
  NDNSF wire messages.
- **FR-008**: The ONNX adapter SHALL derive tensor dependencies and candidate
  cut information from the model graph rather than requiring users to handwrite
  pipeline edges.
- **FR-009**: PyTorch, container, LLM, and other non-ONNX model families SHALL
  be able to provide the same common splitter output through adapters.
- **FR-010**: NDNSF-DI SHALL provide an operator-facing pre-split API that can
  register, inspect, query, and retire immutable pre-split entries without
  granting planning strategies mutation authority.
- **FR-011**: Each pre-split entry SHALL bind the exact model and tokenizer or
  preprocessing semantics, splitter identity, chunk graph, artifact names and
  digests, runtime and backend requirements, role resource requirements, and
  lifecycle status.
- **FR-012**: The built-in `PreSplitFirstStrategy` SHALL first prefer a feasible
  exact immutable pre-split entry and, within equivalent plans, rank fresh exact
  residency in this order: GPU with a valid reuse pin or lease, reload-feasible
  unpinned GPU, host RAM, local disk, repository-only, then new materialization.
  When no feasible entry exists, its default behavior SHALL derive a new
  model-specific `SplitSpecification` from the closed ACK snapshot and ask the
  trusted coordinator to materialize and publish it; pre-splitting SHALL be an
  optimization, not a prerequisite. The baseline partitioner SHALL use
  contiguous graph-valid units and estimated per-segment runtime peak
  (weights, activations, KV/workspace, transient overhead, and safety margin),
  not serialized file size alone. Near-equal division is permitted only for
  homogeneous units and equal usable capacity envelopes. Unknown unsafe bounds
  or unsupported model semantics SHALL fail closed.
- **FR-013**: ACK collection SHALL have no model splitting, artifact fetching,
  model loading, warmup, or execution side effect. An atomic, bounded
  NDNSF-DI admission hold used to make the advertised GPU budget exclusive is
  permitted and SHALL NOT perform model preparation. NDNSF Core MAY bind an
  opaque generic lease proof to the exact ACK payload but SHALL NOT interpret
  GPU units or DI compatibility.
- **FR-014**: Within the signed `DI_PROVIDER_OFFER_V2` payload, a generic
  positive ACK SHALL be the Provider's authenticated, request/attempt-bound
  willingness to accept one final Selection for any compatible non-empty set
  of roles whose aggregate resource requirements fit the DI offer envelope.
  Generic `ACK.status=true` alone does not acquire this meaning for other
  services. The baseline DI capacity rule SHALL require the aggregate estimated
  peak GPU RAM assigned to that Provider to be no greater than the offered or
  reserved GPU RAM. The DI offer SHALL bind its exact profile/minimum version,
  requester, service, request and attempt, model intent, Provider identity and
  boot epoch, monotonic resource sequence, freshness, accepted deadline,
  versioned acceptance-predicate and capability/resource digests, and the
  Core-bound one-time ProviderToken; it SHALL NOT require predeclared role
  identities. Its offered GPU budget SHALL be exclusive in the DI admission
  ledger until Selection, explicit release, or expiry so concurrent requests
  cannot consume the same capacity promise. The offer SHALL also carry a
  bounded, signed shard-residency inventory for GPU, host RAM, local disk, and
  repository-only state. Every entry SHALL bind artifact and manifest digest,
  model name, content digest, semantics digest, layer or graph range, backend,
  precision/runtime
  ABI, trust-policy identity, byte count, device when applicable, Provider boot
  epoch, cache epoch, observation time, expiry, and eviction/reload status.
  A hard placement guarantee SHALL rely either on a bounded reuse pin/lease
  through Selection expiry or on a separately feasible reload path; an
  unpinned cache claim alone SHALL never make a plan feasible.
- **FR-015**: Exactly one immutable logical positive final-Selection
  identity/digest per selected Provider SHALL directly carry that recipient's
  exact sealed-plan projection, including the
  complete non-empty role tuple, per-role model fragments, artifacts,
  dependencies, backends, GPU-memory requirements, deadlines, status bindings,
  and input-key grants when applicable. Core SHALL authenticate the generic
  Selection identity, exact opaque payload digest, ProviderToken, optional
  opaque lease, deadline, and replay state without parsing the tuple.
  NDNSF-DI SHALL validate the whole tuple and aggregate resource requirement
  against its positive DI offer, then use one crash-atomic Provider transaction
  to consume the token/lease, commit its GPU admission record, persist and
  install the tuple/grants, publish a mandatory authenticated per-Provider
  Selection-acceptance record, and start request-scoped local preparation
  asynchronously for its roles. An exact resident shard MAY satisfy local model
  preparation immediately only after Selection-time revalidation. Otherwise
  the Provider SHALL fetch the immutable named shard through NDN from
  NDNSF-DistributedRepo, verify signer, manifest, digest, schema, trust policy,
  bounds, and revocation status, stage it atomically to local disk, and load it
  through host RAM to the assigned GPU as required.
  Incremental same-attempt role additions SHALL
  be rejected. Byte-identical retransmission under the same identity/digest
  SHALL be idempotent and SHALL NOT reinstall the tuple or consume the token
  twice. There SHALL be no `PreparationCommit`, preparation token, commit
  receipt, second role-negotiation decision, or second ACK round. The mandatory
  per-Provider Selection-acceptance record is linearization evidence for the
  first Selection decision, not another willingness decision.
- **FR-016**: Final inference Selection SHALL remain in the original request
  and attempt and SHALL bind the exact validated ACK snapshot, placement-plan
  identity, Provider boot epoch, assignment, resource envelope, and still-valid
  ProviderToken without repeating Request publication or ACK collection. It
  SHALL NOT wait for any complete set of local-ready observations. Role-local
  readiness SHALL be reported separately and bind the resulting Selection.
- **FR-017**: A provider role SHALL begin inference execution if and only if it
  has validated final Selection for the matching plan and assignment, its own
  model/runtime preparation is fresh and complete, and every declared direct
  input is available and authenticated. A transition of any one of these
  conditions SHALL re-evaluate the role locally; no complete-system readiness
  set or extra activation message is permitted. The first eligible transition
  SHALL atomically fence `WAITING -> RUNNING` by request, attempt, plan, role,
  provider boot epoch, and execution generation so duplicate or concurrent
  events cannot start the role twice.
- **FR-018**: Missing, negative, stale, expired, wrong-boot, or conflicting ACK
  evidence, or an assignment outside its positive-ACK envelope, SHALL prevent
  positive final `SELECTED` for that assignment. Incomplete local readiness
  alone SHALL NOT prevent Selection. Local-preparation or dependency failure
  after Selection SHALL trigger bounded, idempotent dependency-closure
  compensation, cancellation, release, or abort according to the sealed plan.
  Once an attempt is failed, cancelled, expired, or superseded, late readiness,
  input, output, and status events from that attempt SHALL NOT restore
  eligibility, wake consumers, or form a terminal Response.
- **FR-019**: Existing NAC-ABE authorization, provider permissions, one-time
  user/provider tokens, signature verification, replay protection, and response
  verification SHALL remain mandatory across planning, Selection, preparation,
  execution, compensation, and result handling. One inference request SHALL
  use a request-scoped UserToken plus the existing one-time ProviderToken for
  each positive Provider ACK. Final Selection SHALL consume that ProviderToken
  and any ACK-carried admission lease, and only final Selection may grant
  input-key access.
- **FR-020**: One durable invocation SHALL contain a bounded sequence of
  attempt state machines. Attempt status SHALL distinguish request-published,
  ACK-collecting, ACK-closed, planning, materializing, plan-sealed, active,
  succeeded, failed, cancelled, expired, and superseded outcomes; a terminal
  attempt SHALL never return to active. During `active`, per-Provider Selection
  delivery SHALL independently report pending, unknown, selected, not-selected,
  failed, cancelled, or expired, while non-exclusive request observations
  report Selection-partial/complete and dataflow activity.
  Per-role status, created only after Selection installs an assignment, SHALL
  independently report local preparation, input readiness, runnable,
  executing, output publication, and terminal state with durable reason
  evidence. No status model SHALL force Selection delivery and role execution
  into one global phase order.
- **FR-031**: Trusted NDNSF-DI materialization and repository-publication
  services SHALL execute only after a valid strategy decision and before plan
  sealing and final Selection. The data-only decision SHALL carry an explicit,
  validated `GENERATED` or `PRE_SPLIT` artifact-preparation mode; the trusted
  coordinator SHALL NOT infer side effects from free-form evidence.
  `GENERATED` SHALL invoke materialization then publication, while `PRE_SPLIT`
  SHALL resolve and revalidate an existing complete publication without
  rematerializing it. Publication SHALL use immutable,
  content-addressed names and a signed manifest; partial staging SHALL not
  become selectable. Materialization or publication failure SHALL yield zero
  selected execution. Successfully published model/runtime artifacts MAY
  survive requests under bounded cache policy, but request inputs, outputs,
  activations, temporary plaintext, grants, and every mutable state class
  lacking an explicit reusable `InferenceStateContract` SHALL be destroyed or
  released at terminal convergence. Reusable derived state remains governed by
  FR-035–FR-037 and SHALL NOT become a model-repository artifact. A selected, in-flight, or explicitly
  reuse-pinned shard SHALL not be evicted; eviction of other entries SHALL
  update the cache epoch and later ACK state.
- **FR-021**: The dependency-driven executor SHALL support chain, fan-in, and
  fan-out role graphs, combine role-local preparation readiness with all
  declared authenticated direct inputs, re-evaluate a role whenever either
  condition changes, admit each role at most once per generation, publish
  outputs that wake
  only direct consumers in the same live attempt, reject old-attempt events,
  and reject partial terminal outputs.
- **FR-022**: The new ACK-offer/Selection-assignment dataflow path SHALL be
  explicitly versioned as an authenticated NDNSF-DI payload profile, currently
  `SELECTION_DATAFLOW_V2`, and SHALL NOT be added as a DI-specific Core
  invocation mode or parallel wire lifecycle. Its canonical carrier SHALL be
  NDNSF's existing generic distributed-collaboration API, Selection,
  `CollaborationContext`, collaboration object transport, status reporting,
  and final Response. The Request binds the required profile/minimum version
  and allowed fallback; capability stripping or silent V1 downgrade SHALL fail.
  One attempt SHALL NOT mix V2 positive-offer/asynchronous-preparation semantics
  with the legacy Selection-triggered-synchronous-loading path.
- **FR-023**: The canonical application API SHALL invoke planning as part of the
  normal single-call inference lifecycle. Its request SHALL accept an immutable
  `ModelRef` containing `model_name`, canonical signed-manifest
  `content_digest`, and tokenizer/config/preprocessing `semantics_digest`, plus
  input, timeout, and optional objective/constraints. Callers SHALL NOT provide
  a deployment, Provider list, pre-split, separate preparation request, or map
  of internal policy requests. `app.yaml` SHALL explicitly configure local
  identity, trust, state, repository prefix, default strategy/model adapter,
  and cache policy; it SHALL NOT encode a per-request model identity, role
  assignment, Provider list, secret key, or precomputed deployment.
- **FR-024**: Every accepted request SHALL retain the strategy identity and
  state, validated ACK snapshot digest, split or pre-split identity, placement
  plan digest, ACK offer/resource evidence, final Selection identity,
  role-local-ready receipts, per-role latch/timeline evidence, execution
  status, and compensation outcome.
- **FR-025**: The default feature-validation path SHALL use MiniNDN and an exact
  frozen `ModelRef` for `Qwen/Qwen3-0.6B` with exact content and semantics
  digests, the smallest Qwen model selected for
  this gate. TigerCluster and a larger Qwen model SHALL be used only after an
  explicit, separately recorded authorization; they are not a normal Spec 163
  exit gate. If the MiniNDN host lacks CUDA, network, split/publication,
  integrity, lifecycle, and CPU/host-cache behavior MAY be validated there,
  but GPU-residency and GPU-reload claims SHALL remain explicitly deferred
  rather than simulated.
- **FR-026**: The canonical V2 implementation SHALL keep every DI schema,
  codec, validator, GPU admission ledger, role tuple, preparation state,
  tensor/object contract, result contract, and recovery rule inside
  `NDNSF-DistributedInference`. Base NDNSF MAY add only the generic,
  DI-opaque deferred-collaboration handle and `ACK_CLOSED`/one-shot-plan-commit
  state needed by any collaboration whose roles are chosen after discovery.
  Existing Core deployment/READY/activation
  behavior SHALL be classified as frozen V1 debt and SHALL NOT be extended.
  A static ownership gate SHALL reject new V2 DI identifiers or field parsing
  in base `ndn-service-framework/` and `pythonWrapper/ndnsf/`, apart from an
  explicit finite legacy allowlist.
- **FR-027**: The implementation SHALL use the trust and threat model in
  `contracts/lifecycle-security-contract.md`. In-process strategies SHALL be
  operator-installed, digest-pinned, allowlisted trusted code; their outputs
  remain untrusted and validated. The feature SHALL NOT claim hostile Python
  sandboxing or Byzantine Provider computation correctness without separate
  evidence. Model adapters, RunnerAdapters, and publishers of executable
  containers, pickles, custom operators, or native libraries SHALL be explicit
  host-TCB members; authentication of their bytes SHALL NOT be presented as
  executable-code sandboxing.
- **FR-028**: Selection installation, ProviderToken/lease disposition, DI GPU
  admission, role/grant installation, and acceptance evidence SHALL have one
  durable Provider-side linearization record. The canonical mechanism SHALL be
  the Core-owned generic `GenericSelectionTxnStore` WAL defined in
  `contracts/core-opaque-selection-transaction.md`: Core atomically owns only
  generic token/opaque-lease disposition and encrypted opaque commit bytes/
  digests, while a registered NDNSF-DI participant purely validates and defines
  the blob, acceptance payload, and idempotent DI projection. Core SHALL NOT
  parse the blob, and token/lease state SHALL NOT commit in an earlier separate
  store. The coordinator SHALL journal plan, exact Selection bytes,
  supersession, cancellation, and terminal Response decisions before their
  corresponding external effects, using encrypted-at-rest ephemeral control
  state or safely aborting/replanning when it is unavailable.
- **FR-029**: Every plan SHALL define exactly one `ResultContract` with one
  response producer or explicit aggregation role covering the complete required
  sink set. Output objects and final Response SHALL use the authenticated,
  bounded, segmented, digest, schema, encryption, signer, lineage, UserToken,
  idempotency, and first-terminal-wins rules in the lifecycle/security contract.
  AEAD SHALL use per-key unique object/segment nonces or an approved
  misuse-resistant construction; public manifests SHALL expose ciphertext
  digests rather than plaintext digests; and the baseline metadata leakage of
  routing prefixes, ciphertext size/segments, timing, and traffic relations
  SHALL be stated rather than treated as confidential. The final result SHALL
  bind the recomputed required Provider/Selection-acceptance-set digest and
  SHALL NOT succeed while any Provider in the result dependency closure is
  `UNKNOWN`, failed, cancelled, expired, or not selected.
- **FR-030**: Cancellation SHALL be a coordinator-local durable terminal
  decision followed by authenticated idempotent Provider convergence and local
  expiry, never a claim of distributed atomic cancellation. Replan SHALL use a
  fresh attempt; an old output is reusable only through explicit
  `AdoptedInputEvidenceV2`, semantic revalidation, and fresh recipient grants.
  Cancel, release, and status query SHALL use access-controlled,
  NDNSF-DI-owned opaque control payloads over the existing generic exact-target
  service/control path; they SHALL NOT add a DI message kind to Core, assign
  roles, extend deadlines, or disclose secrets to unauthorized identities.
  Every attempt, offer, Selection unknown, role, object, and resource SHALL
  reach a bounded terminal or release/expiry state under the documented
  retry/attempt/cleanup budgets and clock-skew model.
- **FR-032**: The canonical dynamic NDNSF-DI path SHALL call a generic NDNSF
  `begin_collaboration` operation before Request publication and SHALL receive
  one immutable, digest-bound `ACK_CLOSED` snapshot from that invocation before
  invoking the DI strategy. After trusted decision validation and any required
  materialization/publication, NDNSF-DI SHALL call `commit_plan` exactly once
  on the same live invocation with generic roles, dependencies, key scopes,
  Provider assignments, artifact Data names, and opaque per-Provider assignment
  payloads. Only NDNSF SHALL emit the existing final Selection and create
  `CollaborationContext`; NDNSF-DI SHALL NOT publish a parallel Request, ACK,
  Selection, collaboration-data, status, or Response protocol.
- **FR-033**: Existing `request_collaboration` calls that supply roles and
  dependencies before Request SHALL remain a versioned preplanned
  compatibility path for already deployed/fixed plans. They SHALL project into
  the same generic collaboration invocation and Selection state machine, be
  observable as `PREPLANNED`, and SHALL NOT be invoked by the default Spec 163
  dynamic planner. The deferred path SHALL be observable as `DEFERRED`, reject
  plan commit before `ACK_CLOSED`, reject a second or conflicting commit, fail
  closed after deadline/cancel/supersession, and preserve byte-identical
  idempotent commit retry after an unknown delivery result.
- **FR-034**: NDNSF-DI SHALL expose a model-family-neutral application request
  accepting an immutable `ModelRef`, an `InferenceTaskRef`, an
  adapter-validated `ApplicationInput`, optional adapter-owned task options,
  deadline, objective, and constraints. A registered `ModelFamilyAdapter`
  SHALL compose independently replaceable graph/split, task-I/O, state, and
  runner ports and SHALL publish a canonical descriptor containing adapter
  identity, version, code/state digest, ABI, supported tasks, input/result
  schema digests, and capabilities. ONNX, text generation, object detection,
  speech, diffusion, and opaque-container adapters SHALL use the same public
  request and generic collaboration carrier. LLM-only concepts such as prompt,
  tokenizer, sampling, logits, tokens, and KV layout SHALL remain inside the
  applicable adapter contracts and SHALL NOT become NDNSF Core fields. An
  unsplittable model MAY expose a one-node opaque `ModelGraphSnapshot`.
- **FR-035**: Every adapter SHALL declare an `InferenceStateContract` that
  classifies each mutable inference-state class as `STATELESS`,
  `REQUEST_SCOPED`, `SESSION_SCOPED`, `EXACT_PREFIX_REUSABLE`, or
  `CUSTOM_ADAPTER_MANAGED`, together with identity inputs, ownership, resource
  accounting, allowed persistence and migration, expiry, eviction,
  confidentiality, authorization, revalidation, and cleanup rules. Absence of
  an explicit reusable-state contract SHALL mean destroy at terminal
  convergence. Immutable model/runtime artifacts SHALL remain a distinct cache
  class and semantic response caching SHALL remain out of scope.
- **FR-036**: The LLM exact-prefix KV profile SHALL use an opaque canonical
  state-key digest binding at least the model content and semantics digests,
  adapter and runner ABI/digests, split/manifest and role/layer range, prefix
  token digest, position/context parameters, precision, KV layout, tenant or
  security domain, Provider identity, boot/cache epoch, and expiry. ACK offers
  MAY advertise only bounded authenticated sanitized evidence such as that
  digest, covered layer range, bytes, tier, expiry, and access domain; they
  SHALL NOT disclose prompts, tokens, plaintext KV, keys, or cross-tenant
  membership. A selected reuse binding SHALL be sealed into `commit_plan` and
  final Selection, pinned or reserved, and revalidated immediately before use.
  Mismatch, eviction, restart, expiry, missing authorization, or failed pin
  SHALL produce a clean non-reuse path or structured replan, never approximate
  or silent reuse. Each pipeline role owns KV only for its declared layer
  range; the baseline SHALL require complete compatible coverage of every
  reused range and SHALL NOT infer correctness from a partial multi-stage hit.
- **FR-037**: Reusable derived state SHALL be Provider-local by default and
  SHALL NOT be published as an immutable public model shard. Optional
  cross-Provider migration MAY be enabled only by an adapter-declared state
  migration contract with end-to-end encryption, recipient authorization,
  integrity and lineage binding, replay/epoch protection, bounded size and
  deadline, resource reservation, and source/destination cleanup. Migration is
  an optimization rather than a Spec 163 baseline gate; unsupported,
  uneconomic, or failed migration SHALL fall back to clean computation without
  weakening output semantics.

### Key Entities

- **Model Description**: Human-readable model name, authoritative model-manifest
  content digest, tokenizer/config/preprocessing semantics digest, optional
  source revision provenance, graph summary,
  adapter identity, inputs, outputs, and runtime requirements.
- **Model Family Adapter**: A versioned composition of pure graph/split,
  task-I/O and state-contract ports plus a separately trusted runner port. It
  translates model-family semantics into common immutable planning values
  without receiving Selection, network, repository, or device authority.
- **Inference State Contract**: Adapter-owned declaration of mutable
  request/session/derived state identity, lifecycle, authorization, resource
  cost, persistence, migration, revalidation, eviction, and cleanup.
- **Derived State Cache Entry**: Provider-owned mutable cached state, such as
  exact prefix KV for one layer range, identified by an opaque digest and
  bounded by model/adapter/runtime/split/security/epoch/expiry evidence.
- **State Reuse Binding**: Immutable plan and Selection data authorizing one
  Provider role to revalidate and use one exact derived-state entry.
- **Provider Capability Snapshot**: Validated ACK metadata and measurements
  bound to provider identity, boot epoch, service, request, freshness, and
  capability/resource envelope. A positive snapshot expresses willingness for
  any compatible role tuple within the advertised aggregate GPU-memory bound
  rather than predeclared role identities.
- **Split Candidate**: A side-effect-free proposed chunk graph with artifact
  recipe, tensor boundaries, resource requirements, estimated costs, and model
  semantics.
- **Pre-Split Entry**: An immutable, operator-managed materialized split and
  artifact manifest available to planning as a read-only candidate.
- **Placement Decision**: The strategy result selecting one split and assigning
  every role to an eligible provider under one exact input snapshot.
- **Deferred Collaboration Invocation**: The generic NDNSF-owned durable handle
  that publishes one Request, closes one immutable ACK snapshot, accepts one
  generic plan commit, drives existing Selection/collaboration transport, and
  reaches one bounded terminal state without interpreting DI payloads.
- **DI Collaboration Plan**: A validated and sealed NDNSF-DI execution contract containing
  roles, dependencies, artifacts, object naming, prefetch rules, execution
  order, and compensation policy. NDNSF-DI projects its generic role,
  dependency, key-scope, assignment, and artifact-name fields into the existing
  NDNSF `CollaborationPlan` only when committing the deferred invocation.
- **Provider ACK Offer**: Private positive-ACK material containing the existing
  one-time ProviderToken, optional admission/resource lease, Provider boot
  epoch, accepted deadline, and status handle. Strategies receive only its
  sanitized capability/resource snapshot.
- **Provider Selection Assignment**: The recipient-specific projection of the
  sealed plan carried by one final Selection to one Provider. It atomically
  binds that Provider's complete role tuple, per-role fragments, artifacts,
  dependencies, backends, aggregate GPU-memory requirement, deadline, and ACK
  evidence and directly starts request-scoped asynchronous role-local
  preparation while permitting reuse of eligible prewarm/cache work.
- **Role Local-Ready Receipt**: Authenticated evidence that one assigned
  model/runtime instance is locally executable under a bounded lease. It
  satisfies only that role's local-preparation latch.
- **Input Availability Evidence**: Complete authenticated request input or
  direct-predecessor output references satisfying one role's declared input
  edges. It is independent of local model readiness.
- **Execution Evidence**: Dependency receipts, operation status, outputs,
  terminal result, and compensation records bound to the selected plan.
- **Inference Invocation**: One public durable request handle, original total
  deadline, bounded attempt budget, and one first-terminal-wins result.
- **Attempt**: One wire Request/ACK closure, optional materialization, one
  sealed plan, per-Provider Selection delivery, role generations, and a
  monotonic terminal state. Reassignment creates a new attempt.
- **Selection Acceptance Record**: Mandatory authenticated per-Provider evidence
  that one exact Selection transaction consumed its token/lease and installed
  the complete DI role tuple. It is not a readiness receipt or global cover.
- **Input/Output Object Manifest**: Bounded, signed, digest- and
  schema-constrained segmented object lineage with producer/consumer,
  encryption, expiry, and model-semantic bindings.
- **Result Contract**: One response producer or explicit aggregation role plus
  the complete required sink-output set and final Response validation rules.
- **Adopted Input Evidence**: Trusted revalidation and fresh-attempt
  authorization for a completely published old-attempt output; late old events
  without this evidence remain rejected.

## Success Criteria

### Measurable Outcomes

- **SC-001**: An external test strategy changes both the selected split and at
  least one provider-role assignment without modifying NDNSF runtime,
  artifact-transfer, or executor source.
- **SC-002**: For every frozen feasible pre-split case, the default strategy
  chooses the exact entry and the highest-ranked safe residency tier; it
  chooses zero stale, incompatible, incomplete, unbounded, or digest-mismatched
  entries. For every feasible no-pre-split case, it deterministically emits a
  graph-valid, capacity-safe `SplitSpecification`, and the resulting immutable
  repository manifest is sealed before any Selection.
- **SC-003**: Repeating planning with byte-identical canonical inputs and
  strategy state produces the same plan digest in 100% of deterministic test
  cases.
- **SC-004**: All negative tests for stale ACKs, unknown providers, invalid
  graphs, artifact mismatch, strategy timeout, tampered ACK resource envelopes,
  boot-epoch changes, ProviderToken/admission-lease replay, out-of-envelope
  Selection, concurrent cross-request GPU-budget double commitment, incomplete
  or incremental same-Provider role bundles, input-key grant tamper or
  cross-role/cross-attempt replay, early cleanup timers,
  cancel-versus-last-input, failure-versus-local-ready,
  deadline-versus-execution-start, old-output-after-replan, and partial
  final-Selection delivery fail before unauthorized or partial terminal
  execution is accepted.
- **SC-005**: Across missing, negative, stale, expired, wrong-boot, or
  insufficient-GPU-RAM ACK tests, and every out-of-envelope assignment test,
  zero unauthorized positive final `SELECTED` decisions occur. Incomplete local
  readiness does not block Selection backed by validated DI offers carried by
  positive generic ACKs. For every
  role, execution remains zero until that same role has valid Selection, fresh
  local preparation, and all authenticated direct inputs; other eligible roles
  may execute independently. All failed or cancelled resources reach a
  terminal release state.
- **SC-006**: Across successful tests, each selected Provider receives exactly
  one atomic Selection containing its complete role tuple; that tuple fits the
  corresponding validated DI offer's capability and aggregate GPU-memory
  envelope and consumes exactly one existing ProviderToken. Every role is
  admitted at most
  once per generation using the selected plan after its own model and direct
  inputs are ready, and only one terminal result is accepted for the invocation;
  preparation and inference retain one request/attempt identity with no
  `PreparationCommit`, second Request, or second ACK round.
- **SC-007**: One chain workload and one real fan-in/fan-out workload execute
  through the same dependency-driven executor and match their frozen local
  reference outputs. The chain proves Stage 0 can execute while a downstream
  stage is still preparing, and each downstream stage starts automatically
  when the later of local readiness and its last required input occurs.
- **SC-008**: The default MiniNDN acceptance uses a frozen
  `Qwen/Qwen3-0.6B` `ModelRef` with exact content and semantics digests and five
  real prompts of at most 64 generated
  tokens. After one unmeasured warmup, each prompt has five measured
  repetitions. It preserves complete answers, reference-consistency results,
  TTFT, per-token latency, total latency, tokens/s, split/materialization,
  repository publication/fetch, disk/RAM/GPU preparation, cache tier and hit
  reason, bytes fetched, utilization, success, and recovery distributions.
  Per-token timings, when retained, are internal stage data-plane measurements;
  they do not authorize or require a per-token NDNSF Request/ACK/Selection
  cycle. The normative application result is one complete Response per
  generation invocation. A large complete Response may be segmented and
  reassembled by the transport under the same invocation lineage; those
  segments are not additional Responses or collaborations.
  The cold pass starts with empty Provider caches. An exact warm GPU-reuse pass
  succeeds only if it records zero re-split, zero re-publication, zero model
  bytes fetched from the repository, and zero GPU shard reload for every
  selected exact-resident shard; lower-tier reuse is reported separately.
- **SC-009**: The public application workflow requires only an immutable model
  reference, input, objective or constraints, and an operator-configured
  optional strategy;
  it requires no hand-authored provider role graph or low-level NDNSF message.
  The model reference exposes a human-readable model name plus authoritative
  content and semantics digests; a name or moving revision alone is
  insufficient for cache or pre-split reuse.
- **SC-010**: No TigerCluster or large-model job is a default feature gate, and
  zero such jobs are submitted without explicit separate authorization and a
  recorded experiment requalification.
- **SC-011**: Static ownership checks report zero new V2 model, GPU, role,
  artifact, tensor, DAG, preparation, result, or recovery parsing in base NDNSF
  Core outside the frozen legacy allowlist. A non-DI fixture passes the same
  opaque payload/token/lease/status Core seam, while all DI semantic tests pass
  entirely through `NDNSF-DistributedInference`.
- **SC-012**: A model-based or property-based history oracle reports zero
  violations of Selection acceptance, capacity exclusion, at-most-once
  role-generation admission, first-terminal-wins, complete-result, old-attempt
  fencing, and bounded-release invariants across duplicate, loss, reorder,
  partition, downgrade, malformed-object, and crash-at-every-linearization-point
  schedules within declared finite state/history bounds. Exhaustive finite
  cases state their bounds; generated cases retain seeds and replayable shrunk
  counterexamples. This is a bounded evidence claim, not a general proof by
  property testing.
- **SC-013**: Static call-path checks and retained integration traces show that
  100% of successful default V2 dynamic requests use one NDNSF generic
  collaboration invocation in `DEFERRED` mode, one immutable `ACK_CLOSED`
  snapshot, and one accepted plan commit; they use the existing Selection,
  `CollaborationContext`, collaboration-object/status, and Response paths and
  introduce zero parallel DI wire names or duplicate collaboration state
  machines. A separate compatibility test proves the current preplanned
  roles/dependencies form still works and is reported as `PREPLANNED`, while
  the default dynamic path invokes it zero times.
- **SC-014**: One LLM text-generation adapter, one object-detection adapter,
  and one opaque single-node container adapter pass the same public request,
  model identity, ACK-closed planning, `commit_plan`, Selection, object, status,
  and Response contracts. Static checks find zero LLM-specific branches or
  fields in base NDNSF Core and zero requirement that every model expose
  splittable internals.
- **SC-015**: A deterministic local/MiniNDN state matrix covers cold execution,
  request-scoped cleanup, exact authorized local prefix-KV hit, model,
  semantics, adapter, runner, split, layer, token-prefix, position, precision,
  layout, tenant, epoch, expiry, and eviction mismatches, Provider restart,
  failed pin, and migration-disabled fallback. Every exact hit preserves the
  frozen reference result and records avoided covered-prefix prefill; every
  mismatch performs clean computation or structured replan with zero
  cross-tenant disclosure or unauthorized reuse.

## Assumptions

- The dissertation scope is the automatic NDNSF-DI collaboration plan,
  reference-based artifact exchange, provider assignment, dependency-driven
  chain/fan-in/fan-out execution, and failure compensation described in
  `docs/PAPER/proposal-defense/main_ch.pdf`.
- This feature provides a baseline strategy and extension API; it does not need
  to solve globally optimal partitioning, every horizontal/vertical parallelism
  method, training, federated learning, tensor-parallel kernels, or every model
  family.
- NDNSF Core remains application-independent. Model graphs, split candidates,
  artifact manifests, placement objectives, and preparation orchestration
  belong to NDNSF-DI unless a reusable Core primitive is demonstrably required.
- The new single-request path reuses generic positive-ACK payloads, the existing
  one-time ProviderToken, optional generic admission leases, pending-state
  lifetime, final Selection assignment payloads, and request/role-scoped
  lifecycle status. Core does not parse model, split, artifact, backend, or
  role-preparation semantics, and no fifth generic
  Request/ACK/Selection/Response message kind is implied.
- The first default strategy uses only capabilities, usable capacity,
  runtime-peak bounds, residency, freshness, reuse-pin, reload, and network
  evidence present in validated inputs. Equal partitioning is a deterministic
  special case for homogeneous graph units and equal usable GPU envelopes, not
  a general file-size rule; richer prediction or optimization algorithms can
  be supplied through the same extension point.
- Strategy output is data-only and validation is authoritative. In-process
  strategy code is operator-trusted rather than sandboxed; artifact
  materialization, provider preparation, Selection, execution, and compensation
  are owned by trusted NDNSF-DI coordinators.
- Model-family graph/split, task-I/O, and state adapters produce bounded data
  contracts and do not receive network, Selection, repository, or device
  authority. RunnerAdapters and authorized publishers of executable
  artifacts are trusted members of the Provider host TCB. Signatures and
  digests authenticate their bytes but do not make a malicious container,
  pickle, custom operator, or native library safe; untrusted executable content
  requires a separately specified process/VM sandbox and format policy.
- Pre-existing immutable model/runtime cache residency is an optimization and may persist
  across requests under operator policy. Exact request-scoped preparation
  begins after final Selection, may complete immediately by revalidating a
  compatible cached shard, and can never become a complete-system readiness
  barrier. GPU/RAM claims die on Provider restart; disk entries survive only
  after re-verification. Request inputs, outputs, activations, and grants never
  become cross-request model-cache entries. Mutable inference state is destroyed
  by default; only state explicitly classified and authorized by an
  `InferenceStateContract` may survive under a separate Provider cache policy.
  Exact prefix KV reuse is local and same-security-domain by default, and is
  never treated as immutable model-artifact reuse.
- Exact-target final Selection delivery is not cross-Provider atomic. The
  framework bounds and fences partial delivery and rejects partial terminal
  results; it does not claim simultaneous multi-Provider start or distributed
  atomic commit.
- An authenticated Provider signature proves origin and integrity, not correct
  computation by a malicious authorized Provider. Byzantine-result detection
  requires optional redundancy, attestation, or application-level verification
  and is not silently claimed by this feature.
- Physical computation may repeat after a crash or new attempt. The guaranteed
  property is at-most-once admission per role generation and exactly one
  accepted terminal result for the invocation.
- Existing experiment identities and failed evidence remain immutable while the
  architecture changes. Pausing future work does not relabel prior failures.
