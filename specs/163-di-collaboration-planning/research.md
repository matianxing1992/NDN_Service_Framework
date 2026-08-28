# Research Decisions: Pluggable DI Collaboration Planning

## Model-family neutrality and composed adapters

**Decision**: The canonical application request is model-family-neutral and
resolves a versioned `ModelFamilyAdapter` that composes graph/split, task-I/O,
state, and runner ports. ONNX is one graph adapter, Qwen text generation is one
task/state/runner composition, and vision, speech, diffusion, or opaque
containers use the same request and collaboration carrier. An adapter without
safe internal visibility emits an unsplittable one-node graph.

**Rationale**: The current deployment-oriented application method and narrow
`RunnerAdapter.supports/create_runner` seam cannot express dependency graphs,
input/output semantics, resource/state contracts, or safe split behavior for
multiple model families. A single giant adapter would also mix pure planning
data with executable authority. Composed ports preserve one common contract
while keeping materialization, publication, Selection, and device effects in
trusted coordinators/runners.

**Rejected**:

- Put prompt, tokens, sampling, logits, or KV fields in NDNSF Core. Rejected
  because this would bind the generic carrier to LLM inference.
- Require every model to expose an ONNX-like DAG. Rejected because opaque or
  proprietary runtimes may be safely runnable but not safely splittable.
- Give one adapter object network, repository, Selection, and device authority.
  Rejected because it enlarges the host TCB and defeats independent validation.

## Mutable inference state and exact prefix KV

**Decision**: Distinguish immutable model/runtime artifact cache,
request/session mutable inference state, reusable exact derived state, and
semantic response cache. Spec 163 defines a general `InferenceStateContract`;
LLM prefix KV is the first `EXACT_PREFIX_REUSABLE` profile. Provider-local
reuse is the baseline. Optional encrypted migration is a separate adapter
capability and not a feature gate.

**Rationale**: Destroying all KV at every terminal request prevents useful
exact prefix reuse, while treating KV like a public immutable model shard is
incorrect: KV is input-derived, mutable, layer/range-specific, security-domain
bound, and invalidated by model, tokenizer, runner, split, position, precision,
layout, epoch, or expiry changes. The plan and Selection must bind reuse, and
the Provider must pin and revalidate it immediately before use.

**Security decision**: ACK exposes only authenticated bounded opaque
state-key evidence; no prompt, tokens, plaintext state, keys, or unrestricted
cache membership. Cross-tenant reuse is denied by default. Exact content
identity alone does not confer authorization.

**Rejected**:

- Approximate or semantic prefix matching. Rejected because equivalent-looking
  text or token counts do not prove numerically compatible KV.
- Publish KV under NDNSF-DistributedRepo like model chunks. Rejected because
  default repository caching and replication do not provide the required
  access-domain, mutability, and expiry semantics.
- Treat an ACK cache hit as durable proof. Rejected because state may be
  evicted or invalidated before Selection; pin/reservation and revalidation are
  mandatory.

## Scope authority

**Decision**: The controlling research scope is Chapter 5 and the RQ2/RQ5
evaluation matrix in `docs/PAPER/proposal-defense/main_ch.pdf`.

**Rationale**: That dissertation proposal defines NDNSF-DI as an
artifact-intensive, multi-stage provider-collaboration workload. Its proposed
work is automatic collaboration-plan generation, a common splitter output,
reference-based artifact exchange, provider assignment, dependency-driven
chain/fan-in/fan-out execution, and failure compensation.

**Alternatives considered**:

- Treat the broader project proposal in
  `docs/PAPER/reference-pdfs/ai_proposal.pdf` as the personal dissertation
  contract. Rejected because it assigns unrelated project-wide horizontal and
  vertical model-optimization research to this feature.
- Reduce NDNSF-DI to the current Qwen pipeline. Rejected because the
  dissertation explicitly requires model-independent collaboration evidence.

## One joint placement extension point

**Decision**: Make one `ModelPlacementStrategy` the canonical external
extension point. It selects one split candidate and assigns every required role
to a provider in one atomic decision.

**Rationale**: Split selection and provider assignment are coupled. A cut that
is feasible for one set of providers may be infeasible or expensive for
another. The current ten-policy SDK separates partition and assignment and
cannot express or validate one joint choice cleanly.

**Alternatives considered**:

- Keep independent public partition and provider-assignment policies. Rejected
  because their results can be individually valid but jointly infeasible.
- Let external code construct the complete NDNSF collaboration plan. Rejected
  because it would expose names, security state, tokens, and execution
  authority outside the framework.

## Splitter and strategy ownership

**Decision**: A model-specific `ModelSplitter` analyzes a model and exposes
side-effect-free graph-valid units, runtime-peak estimates, and split
constraints in one common format. A placement strategy consumes those facts
and the closed validated Provider snapshot, then returns either an existing
manifest reference or a new data-only `SplitSpecification`. Trusted framework
code materializes and publishes only the selected generated specification
through NDNSF-DistributedRepo.

**Rationale**: ONNX, PyTorch, container, and LLM models need different graph
analysis and artifact production, while placement and execution require a
stable model-independent contract. Keeping materialization outside the strategy
also makes policy execution bounded and safe.

**Alternatives considered**:

- Require the strategy to parse and split every model format. Rejected as an
  unstable and unnecessarily broad plugin contract.
- Materialize every candidate before planning. Rejected because it creates
  expensive side effects for candidates that will not be used.

## Pre-split-first default

**Decision**: Provide an operator-managed immutable `PreSplitCatalog` and a
built-in `PreSplitFirstStrategy`. The default considers exact feasible
pre-split entries first and ranks exact fresh GPU residency ahead of RAM, disk,
repository-only, and new materialization. When no feasible entry exists, the
default uses the configured named model-specific splitter to derive a
`SplitSpecification` after ACK closure. Pre-splitting is an optimization, not a
requirement.

**Rationale**: This preserves prepared-stage reuse while removing the
operational requirement to split before a request. A first request may pay
split, publication, fetch, and load costs; later exact-compatible requests can
reuse verified Provider cache state. The strategy still does not pretend that
one universal splitter supports every model family.

**Alternatives considered**:

- Infer shard count from serialized model bytes divided by nominal GPU RAM.
  Rejected because weights, activations, KV/workspace, transient allocations,
  fragmentation, and safety margin determine runtime peak.
- Automatically invoke an unspecified universal splitter when no catalog entry
  matches. Rejected; the default requires a configured named adapter and fails
  closed when that adapter lacks a safe bound.
- Store current provider residency in the static catalog. Rejected because
  residency is dynamic ACK state tied to provider boot epochs.

## Capacity-safe baseline partitioning

**Decision**: The baseline policy partitions graph-valid contiguous units using
an estimated per-segment runtime peak:

```text
weights + activations + KV/workspace + transient overhead + safety margin
```

It assigns segments against each validated offer's usable envelope while
minimizing the maximum normalized load, critical-path transfer, and unnecessary
shard count. Near-equal contiguous partitions are a deterministic special case
only when graph units and usable GPU envelopes are homogeneous.

**Rationale**: For example, 30 GB of serialized weights and 12 GB GPUs imply
only a lower bound of three devices; they do not prove that three runtime
stages fit. A fourth stage may be required once runtime peak and headroom are
included. This formulation is simple enough for a reference policy while
remaining academically defensible and replaceable.

## Provider cache evidence and warm reuse

**Decision**: `DIProviderOfferV2` carries a bounded signed inventory of exact
shard residency in GPU, host RAM, local disk, or repository-only state. Entries
bind model/tokenizer and artifact/manifest digests, graph range, backend,
precision/runtime ABI, trust policy, size, device, boot epoch, cache epoch,
observation time, expiry, pin/lease, eviction, and reload facts.

Hard feasibility may use a resident shard only if a bounded pin protects it
through Selection expiry or if the plan remains feasible through reload.
Selection revalidates the claim. Verified immutable model/runtime shards may
remain after a request under bounded cache policy; request inputs, outputs,
activations, plaintext, grants, and mutable state without an explicit reusable
`InferenceStateContract` may not. Exact prefix KV follows the separate derived
state decision above and never becomes immutable shard residency.

**Rationale**: Residency is a time-varying hint unless eviction and restart
races are bounded. Exact identity and epoch binding prevents stale or poisoned
cache claims from selecting the wrong model. Separating model cache from
request state preserves confidentiality and avoids cross-request leakage.

## Split publication boundary

**Decision**: A strategy remains pure. After validating a generated
`SplitSpecification`, the trusted coordinator invokes `SplitMaterializer` and
`DistributedArtifactPublisher`. The latter publishes content-addressed
segments and a signed immutable manifest through NDNSF-DistributedRepo.
Publication and manifest sealing complete before final Selection; Providers
then fetch assigned shards by NDN after Selection unless exact residency is
revalidated.

**Rationale**: Allowing a strategy to write the repository would turn an
external policy into a privileged artifact publisher. Selecting before a
complete manifest exists would authorize execution of unavailable or partially
published content.

## Strategy trust boundary

**Decision**: Strategy execution is deterministic where declared, time- and
candidate-bounded, and data-only. The API passes only sanitized model,
candidate, provider, network, objective, and catalog snapshots; it does not
pass prompts, tensors, keys, one-time tokens, writable stores, network clients,
GPU handles, or Selection authority. An in-process Python strategy is an
operator-installed, digest-pinned, allowlisted local extension and is therefore
part of the host trusted computing base. Its returned decision remains
untrusted and is independently validated.

NDNSF-DI does not claim that an arbitrary hostile Python class is safely
sandboxed merely because it implements a narrow ABC. Supporting hostile
third-party code would require a separately specified process/OS sandbox. A
thread timeout is not a security or preemption boundary.

**Rationale**: This distinguishes authority minimization from code isolation.
External researchers can implement and install strategies without receiving
protocol authority, while the security claim remains honest about same-process
Python.

**Alternatives considered**:

- Give plugins direct access to the application client and artifact store.
  Rejected because validation after the fact cannot undo network, file, or GPU
  side effects.
- Claim that an allowlist plus a thread timeout contains malicious Python.
  Rejected because same-process code can import modules, inspect process state,
  and continue running after a future timeout is cancelled.

## Existing NDNSF distributed collaboration API is the canonical carrier

**Decision**: Spec 163 extends and reuses NDNSF's existing generic
`RequestCollaboration`/`request_collaboration`, `CollaborationPlan`, Selection,
`CollaborationContext`, collaboration large-object/data, operation-status, and
final-Response facilities. It does not create a second DI-owned
Request/ACK/Selection protocol.

The generic collaboration API gains a DI-opaque deferred-plan lifecycle:

```text
begin_collaboration
  -> Request
  -> ACK collection
  -> immutable ACK_CLOSED snapshot
  -> caller-owned asynchronous planning/materialization
  -> commit_plan exactly once on the same invocation
  -> existing Selection / CollaborationContext / data / status / Response
```

The current preplanned API, in which `CollaborationPlan.roles` and
`dependencies` are supplied before Request, remains a `PREPLANNED`
compatibility mode. Spec 163 uses `DEFERRED` by default because the model split,
role DAG, and Provider assignments depend on the closed ACK set. Both modes
project into one generic collaboration invocation and Selection state machine.

**Rationale**: The current Core already owns the reusable multi-Provider
collaboration mechanisms required by NDNSF-DI. Reusing them prevents duplicate
request tracking, ACK closure, Selection/token consumption, collaboration-data
transport, status, timeout, and Response state machines. A generic deferred
plan seam also benefits non-DI collaborations whose roles are learned after
capability discovery, while keeping model/GPU meaning outside Core.

**Required API correction**: The existing Python `ack_observer` is
observational and cannot return a plan, and the current
`collaborationAckRoleCoverageSatisfied()` requires predeclared roles. The new
seam must therefore expose an immutable ACK-closure future/handle and a
deadline-bound, one-shot generic plan commit. It must not execute the
DI planner or materializer on the Face event loop.

**Alternatives considered**:

- Continue using the collaboration API only for a single coordinator role and
  run a second private DI orchestration lifecycle behind it. Rejected because
  it duplicates the framework collaboration semantics and weakens end-to-end
  token, deadline, status, and evidence attribution.
- Encode unknown future DI roles as placeholder roles before ACK. Rejected
  because placeholders cannot faithfully express graph-derived role count,
  dependencies, artifacts, or aggregate Provider assignment.
- Add a DI-specific Core request mode. Rejected because the generic
  deferred-plan state is sufficient and keeps Core application-independent.

## NDNSF Core versus NDNSF-DI ownership

**Decision**: The canonical V2 path uses the existing generic distributed
collaboration API and its Request/ACK/Selection/Response messages with opaque
application payloads. NDNSF Core owns the generic collaboration invocation,
immutable ACK closure, one-shot plan-commit state, naming, transport,
permissions, signatures, NAC-ABE, UserToken/ProviderToken, exact-target
Selection, `CollaborationContext`, collaboration data/large objects, opaque
payload-digest binding, generic lease proof/lifetime/consume/release, deadline,
replay, idempotency, final Response, and signed status transport. NDNSF-DI owns
every model, split, capability meaning, GPU-MiB, role tuple meaning, artifact
recipe, preparation, tensor, DAG, result, compensation, and evidence semantic.

Current Core deployment messages, preparation callbacks, complete READY-set
logic, and `ExecutionActivate` behavior are legacy architectural debt. They are
frozen behind a V1 compatibility boundary while Spec 163 moves the canonical
schema, validation, admission ledger, preparation, and executor into
`NDNSF-DistributedInference`. V2 work must not add DI fields or branches to
those Core paths.

**Rationale**: Chapter 5.3 of the dissertation proposal assigns model plan,
role, stage, shard, runtime artifact, backend requirement, and dependency
meaning to DistributedInference; it assigns Face, SVS, NAC-ABE, signatures,
permissions, Selection transport, and wire protocol to NDNSF. Treating the DI
payload as opaque preserves that dependency direction and keeps provider
collaboration reusable by non-DI applications.

**Alternatives considered**:

- Extend `NDNSFMessages`, `ServiceUser`, or `ServiceProvider` with V2 model,
  GPU, role, artifact, DAG, or preparation fields. Rejected because this makes
  the application-independent runtime understand one workload's business
  semantics.
- Delete the current Core deployment path immediately. Rejected because frozen
  Spec 116/129 callers and evidence need an explicit compatibility migration.

## Invocation, attempts, and exactly-once claim

**Decision**: One public inference invocation owns a bounded sequence of wire
attempts. Each attempt has at most one Request, one ACK closure, at most one
sealed plan, and one logical Selection per selected Provider. An attempt may be
cancelled before Request publication; once it reaches `REQUEST_PUBLISHED`, it
has exactly one Request, and once it reaches ACK closure, it has exactly one
closed ACK set. Replanning that needs a fresh assignment creates a new attempt
and fresh ACK/token/lease evidence.

The protocol claims at-most-once execution admission per
request/attempt/plan/role/boot/generation and exactly one accepted terminal
result for the public invocation. It does not claim physical exactly-once
computation across crashes; a pure role may be recomputed in a new attempt.

**Rationale**: Physical exactly-once execution cannot be inferred when a
Provider can crash after computing but before durably publishing an output.
The enforceable safety properties are an atomic role-start admission record,
generation fencing, idempotent publication, and a single requester-side
terminal compare-and-set.

## Failure, security, and liveness authority

**Decision**: `contracts/lifecycle-security-contract.md` is the normative
authority for trust assumptions, authenticated DI envelopes, Selection
acceptance evidence, crash recovery, cancellation, object security, result
acceptance, failure classes, compensation, safety, and conditional liveness.

Cancellation has a coordinator-local durable linearization point and
asynchronous Provider convergence; it is not a distributed atomic operation.
Final Selection acceptance has a mandatory authenticated per-Provider record,
but those records are never collected into a global readiness cover. A
successful plan defines exactly one response-producing or result-aggregation
role. A new attempt may adopt an old output only through explicit verified
adoption evidence and fresh authorization.

**Rationale**: Authentication proves origin and integrity, not instantaneous
remote cancellation, Provider computation correctness, or cross-Provider
atomicity. Explicit safety claims, non-claims, clock/delivery assumptions,
linearization points, and bounded terminal behavior are required for an
academically defensible distributed protocol.

## Positive ACK before Selection; prepare and execute by local dataflow readiness

**Decision**: A generic positive ACK acquires DI offer semantics only when its
opaque payload carries a successfully validated, signed
`DIProviderOfferV2`. That offer is the Provider's authenticated,
request-scoped willingness to accept any later compatible role set that fits
its aggregate GPU-memory envelope. The planner chooses the split and concrete
roles after ACK closure. Exactly one immutable logical final-Selection identity
per selected Provider atomically assigns its complete role tuple, consumes its
one ProviderToken, and starts request-scoped asynchronous provider-local
verify/fetch/load/warm while reusing eligible prewarm/cache work. A role
executes only when it has valid Selection, fresh local preparation, and every
authenticated direct input. A bare `ACK.status=true` has no model, GPU, role,
or split meaning in base NDNSF.

**Rationale**: A separate `PreparationCommit` asks a Provider to confirm a
second time what its validated DI offer already promises. Collecting one
commit receipt from every role also creates a hidden cross-role barrier: a
missing downstream receipt prevents Stage 0 from receiving Selection even
though no model-ready barrier is intended. The necessary safety boundary is
instead:

```text
every plan assignment is backed by a fresh validated DI offer
carried by a positive generic ACK
AND aggregate required GPU RAM fits that offer's reserved GPU RAM
AND one Provider's complete role tuple is installed by one atomic Selection
```

The dependency graph then supplies the local execution boundary. Stage 0 can
run when its own model and request input are ready; Stage 1 begins when its own
model and Stage 0 output are both ready, regardless of unrelated or downstream
stages.

**Alternatives considered**:

- Add a `PreparationCommit`/receipt cover after planning. Rejected because it
  duplicates the binding willingness of the validated DI offer and revives a
  global acceptance barrier under a different name.
- Treat ACK capacity as advisory telemetry only. Rejected because concurrent
  planning could allocate the same GPU RAM twice; a validated DI offer for
  this mode must reserve a bounded amount until Selection or release.
- Require a complete set of local-ready receipts before final Selection.
  Rejected because it serializes preparation and execution across all roles.
- Reintroduce a ReadySet or separate ExecutionActivate barrier. Rejected
  because final Selection supplies role authority and each role can evaluate
  its own local/data latches.
- Let Selection synchronously fetch/load/warm. Rejected because Selection
  should install the assignment and start asynchronous preparation, not block
  until the model is ready.

## Wire-layer boundary

**Decision**: Use one public inference invocation containing bounded
wire-level Request/ACK/Selection/Response attempt carried by one generic NDNSF
collaboration invocation. Each attempt has at most one
inference Request; an attempt reaching publication has exactly one
`requestId`/attempt identity, and an attempt reaching ACK closure closes exactly
one immutable set. NDNSF-DI receives that `ACK_CLOSED` snapshot through the
deferred collaboration handle, plans and completes any trusted publication
outside the Core event loop, then commits one generic plan on the same handle.
A validated `DIProviderOfferV2` carried by a positive ACK binds the DI
profile, requester/service/request/attempt/model intent, Provider boot epoch,
resource sequence, accepted deadline, expiry, and canonical
capability/resource envelope. The generic ACK privately supplies the existing
one-time ProviderToken plus an optional generic admission lease. After ACK
closure and trusted plan sealing, exactly one logical exact-target
final-Selection identity per selected Provider carries its complete
provider-role tuple, consumes that token/lease once, and starts asynchronous
local preparation. Only that Selection installs recipient-encrypted,
per-role/per-input key grants bound into its canonical digest; grant tamper or
cross-binding replay fails closed. Byte-identical wire retransmissions are
idempotent. There is no intermediate control message or role-acceptance
decision. The mandatory `DISelectionAcceptanceV2` is evidence of that
Selection's local linearization, not a second willingness/role-negotiation
receipt or a cross-Provider cover.

All model, split, artifact, backend, role, and readiness semantics remain in
NDNSF-DI. Core continues to supply the generic collaboration handle and mode,
ACK payload/closure, pending-request, ProviderToken, optional admission lease,
one-shot generic plan commit, Selection transport/security,
`CollaborationContext`, collaboration data/large-object transfer, final
Response, and signed operation-status primitives.

**Transaction decision**: Base NDNSF supplies one reusable,
application-independent `GenericSelectionTxnStore` WAL and opaque participant
API. A participant's pure bounded `prepare()` returns an opaque commit blob and
acceptance payload. One Core WAL `COMMITTED` fsync atomically owns generic
ProviderToken/opaque-lease disposition and stores those encrypted bytes/digests
without parsing them. The registered NDNSF-DI `onCommitted()` callback
idempotently projects the GPU ledger, role tuple, grants, and latches from that
record. The WAL blob is the durable logical source of truth; the Python DI
projection is not a second authoritative database.

**Rationale**: This avoids the current unsafe ordering in which native Core can
consume/delete token state before later DI validation, assignment, and dispatch
finish. A generic opaque WAL closes the crash gap without teaching Core any
model, GPU, role, artifact, tensor, or DI-recovery semantic. The exact API,
crash cuts, and rollback are normative in
`contracts/core-opaque-selection-transaction.md`.

**Rationale**: The advisor requires preparation to be part of the inference
call, and the model/task-first high-level API promises one
`APPClient.request()` handle, implemented by
`AutomaticPlanningCoordinator.request()`, spanning cold preparation and
execution. A second Request would duplicate discovery, hide cold-start work
outside the inference lifecycle, and make cancellation, deadline, and evidence
correlation needlessly cross-call. Final Selection already has the
Provider-issued one-time authority needed to install a role.

The baseline implementation inspected when this research was written did not
yet satisfy this decision:

- generic default ACK currently says only `Permission Granted`; the binding
  role-neutral willingness and offer-digest semantics are not uniform;
- Python DI can ACK a provisionable model while the native path currently
  tends to ACK only an already-ready deployment;
- GPU-memory fields exist in runtime metadata, but the native resource probe
  does not yet establish a reliable live VRAM promise;
- ProviderToken proof and Provider-side Selection validation do not yet bind
  the complete ACK capability/resource digest;
- Core currently consumes/deletes token pending state before all DI
  validation/assignment/dispatch completes and has no generic durable opaque
  participant WAL;
- generic admission leases use abstract units rather than assignment-derived
  GPU MiB;
- current `CollaborationAssignment` is singular and does not yet install one
  complete multi-role Provider tuple under one Selection/token;
- current `RequestCollaboration` requires roles/dependencies before Request,
  its `ack_observer` is observational only, and role-coverage closure invokes a
  selector against those predeclared roles; it has no asynchronous
  `ACK_CLOSED` handle or one-shot deferred `commit_plan`;
- current coordinator submission uses the collaboration API for one fixed
  `coordinator` role, while fixed-plan inference passes
  `session.plan.ndnsf_roles()` and dependencies before discovery; neither is
  the Spec 163 dynamic path;
- current request lifecycle does not yet expose Selection delivery and
  role-dataflow activity as orthogonal progress;
- provider pending request/token state is scheduled for cleanup after a short
  fixed interval rather than the bounded total request/preparation deadline.
- the legacy deployment path still has complete READY/activation behavior.

The implementation plan therefore first had to extend the existing generic collaboration
invocation with `DEFERRED` mode, immutable ACK-closure delivery, and one-shot
generic plan commit while retaining `PREPLANNED` compatibility. It must also
extend pending state to a capped request
deadline, make validated DI-offer capacity binding for this mode, validate
Selection against the offer and GPU-RAM budget, convert any lease to committed use,
start local preparation asynchronously from Selection, and preserve
request/role-bound status. These were explicit migration requirements, not
behavior claimed by the research-time baseline. Their completed state and
executed evidence are recorded in `traceability.md`, `audit.md`, and
`evidence/minindn-qwen3.md`; this historical finding list is retained to show
the original gap rather than rewritten as if it never existed.

**Alternatives considered**:

- Use two complete preparation and inference calls. Rejected by the advisor
  because preparation must occur inside the inference invocation; it also
  duplicates ACK discovery and splits one deadline across two request handles.
- Add a second PreparationToken and control round. Rejected because the current
  ProviderToken is consumed exactly once by that Provider's one atomic final
  Selection, which installs its complete role tuple and is the sole concrete
  role-assignment transition for the attempt.
- Allow final Selection to exceed its ACK envelope. Rejected because a positive
  ACK commits only the advertised capability and GPU-memory budget.
- Perform unauthenticated or out-of-band preparation RPC. Rejected because it
  bypasses the framework thesis and existing security mechanisms.

## Legacy migration

**Decision**: Introduce explicit `SELECTION_DATAFLOW_V2` as a versioned
NDNSF-DI payload profile negotiated inside generic Request/ACK bytes, not as a
DI-specific base-Core request mode. One attempt uses either the V2
positive-offer/direct-Selection path with asynchronous role-local preparation
and data-driven execution, or an explicitly compatible legacy path, never
both. Keep the legacy method as a bounded compatibility adapter until callers
and evidence migrate.

**Rationale**: Silently changing Selection semantics risks replay,
double-loading, and mixed-version execution. An explicit boundary supports
incremental migration and rollback.

**Alternatives considered**:

- Change `select()` semantics in place. Rejected because existing Spec 129
  evidence and callers depend on its current behavior.

## Failure compensation

**Decision**: Compensation is declared in the collaboration plan and remains
bounded by the original request deadline and semantics. Missing, stale, or
insufficient validated DI-offer evidence prevents Selection. After Selection,
role-local preparation, physical GPU allocation, input, or execution failure
fences the affected dependency closure; compensation may reuse only
independently verified upstream outputs and may reassign only failed or
unfinished roles using a new attempt and fresh ACK/token/lease evidence.

**Rationale**: This supplies the dissertation's failure-recovery evidence
without accepting partial terminal results or silently changing model
semantics.

**Alternatives considered**:

- Restart the complete workflow for every failure. Retained as the safe default
  when no verified reusable output exists, but not required when the plan and
  evidence justify a smaller recovery closure.

## Experiment boundary

**Decision**: Spec 162 remains historically intact as a separate large-model
campaign. Spec 163 validation defaults to MiniNDN with an exact pinned
`Qwen/Qwen3-0.6B` `ModelRef` with exact content and semantics digests: one
cold-cache dynamic-split/publication pass and a
paired warm-cache pass over five real prompts, one warmup, five measured
repetitions, and at most 64 generated tokens. TigerCluster or a larger Qwen is
used only after explicit separate authorization and requalification; it is not
a normal Spec 163 gate.

**Rationale**: The small real model validates output and end-to-end artifact
behavior without making cluster capacity a prerequisite. The paired design
exposes the cold-start distribution and requires a warm exact-GPU hit to show
zero re-split, re-publication, repository weight fetch, and GPU reload. If the
MiniNDN host lacks CUDA, GPU-specific claims remain deferred rather than being
simulated.

**Alternatives considered**:

- Submit the already corrected replacement job before redesign. Rejected
  because a technical success would still validate the architecture being
  retired.
