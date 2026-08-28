# Feature Specification: NDNSF-DI Core/APP Separation

**Feature Branch**: `Experimental`

**Created**: 2026-07-14

**Status**: Implementation — COMPLETION HOLD (controller ownership and role-entrypoint preflight remediation)

**Input**: User description: "Create an independent Spec 111 for NDNSF-DI
Core/APP Separation, audit and repair it. NDNSF-DI should be a pure
distributed-inference framework; application-owned deployment optimization,
model partitioning, provider selection, scheduling, cache, recovery and engine
optimization must be externally replaceable from Python without editing the
framework. Every selection, scoring, ranking, planning and tuning decision must
have a public Python port, while the current implementation becomes the named
default policy. A process-local application-owned `DistributedInferenceEngine`
must compose those ports over a typed objective, immutable engine snapshot and
validated decision graph. A separate optimization team must be able to use
stable NDNSF-DI APIs, including optional quality-constrained model-variant
selection across Qwen sizes and quantizations. Before large-scale Engine
implementation, the feature must also define requester-coordinated distributed
execution and deployment consistency by reusing existing NDNSF execution
leases, attempt/provider epochs and V2/Targeted paths for prepare/commit/abort,
fencing, crash recovery, network partitions and bounded orphan cleanup."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Use a Stable Distributed-Inference Core (Priority: P1)

An NDNSF-DI integrator can construct a plan, bind roles to providers, execute
dependencies, manage leases and recovery, and obtain execution evidence through
a focused Core surface without importing application façades, model-specific
planners, graphical tools, experiment harnesses, or operator commands.

**Why this priority**: A stable, workload-neutral execution Core is the feature's
primary value and the prerequisite for every later separation.

**Independent Test**: Install and import only the Core surface, execute the
existing deterministic native plan/session smoke path, and prove that its
dependency graph contains no application, planner, model-adapter, GUI,
experiment, or operations imports.

**Acceptance Scenarios**:

1. **Given** a valid execution plan and provider assignment, **When** an
   integrator invokes the Core runtime, **Then** roles execute with the same
   dependency, cache-binding, lease, attempt-epoch, security, and evidence
   semantics as before the separation.
2. **Given** only the Core installation surface, **When** it is imported and its
   smoke validation is run, **Then** no APP, Qwen, ONNX planner, GUI, experiment,
   or operator implementation is loaded.
3. **Given** an invalid, stale, unauthorized, or replayed execution input,
   **When** Core validates it, **Then** it fails closed exactly as the frozen
   pre-separation behavior requires.
4. **Given** a multi-Provider intent, **When** one prepare, revalidation or
   commit operation fails or its response is lost, **Then** no Provider may
   activate without one complete authenticated commit certificate and every
   inert reservation is released or expires within its declared bound.
5. **Given** the requester crashes before or after certificate publication,
   **When** it restarts with the same authorized identity, **Then** it either
   resumes the immutable certified attempt/result rendezvous or advances the
   attempt epoch; stale work and output never become authoritative.
6. **Given** two APPDeployment processes issue duplicate or conflicting actions,
   **When** Providers compare lifecycle epoch, expected previous-state digest,
   owner and action digest, **Then** duplicate actions are idempotent and at most
   one conflicting action is authoritative for that epoch.

---

### User Story 2 - Install an External Optimization Package (Priority: P2)

An optimization team develops and installs a standalone Python package that can
replace every NDNSF-DI policy-bearing decision: deployment, partitioning,
quality-constrained model-variant selection, ordinary and multi-role Provider
assignment, scheduling/dispatch, bounded execution tuning, cache, admission,
recovery and execution-target selection.
Runner creation is independently replaceable through a public adapter registry.
The application explicitly selects the package. No NDNSF-DI source edit,
private-module import, global registry mutation, wire change, framework rebuild
or native-only policy implementation is required. Native CUDA/C++ runners may
still be compiled out of tree, but Python owns their registration through the
public adapter SPI and their selection/configuration through the policy suite.

**Why this priority**: This removes the central policy/mechanism mixture while
retaining Core authority over correctness and recovery, and makes the
separation useful to the independent algorithm-development group.

**Independent Test**: Build and install a standalone fixture wheel outside the
NDNSF-DI source package, load it only through an explicitly allowlisted SDK
entry point, replace all ten public Python policies, independently register a
non-default `RunnerAdapter`, and prove that each
non-default decision reaches its intended invocation point while Core/provider
validators reject stale, infeasible, unauthorized, over-budget or malformed
proposals. A partial suite must resolve every omitted hook to an evidenced named
default. The selected execution target must resolve the registered adapter by
identity. Also build one out-of-tree native runner sample against the public SDK
without changing repository sources.

**Acceptance Scenarios**:

1. **Given** an installed external optimization package, **When** an APP
   explicitly selects its `OptimizationSuite`, **Then** deployment, partition,
   unified Provider assignment, scheduling, execution tuning, cache, admission,
   recovery and execution-target policies are resolved by stable
   name/version/digest and only the implemented policies replace named defaults;
   Runner adapters are resolved separately through the selected instance's
   `ExecutionAdapterRegistry`.
2. **Given** a policy that proposes an ineligible provider, **When** Core applies
   the proposal, **Then** Core rejects it rather than allowing policy to bypass
   telemetry freshness, fragment, lease, memory, or authority invariants.
3. **Given** a model family unknown to Core, **When** an installed model adapter
   supplies a valid plan, **Then** Core executes the plan without learning that
   model family's partition semantics.
4. **Given** the same immutable input, configuration and random seed, **When**
   the external package is replayed, **Then** it produces the same decision
   digest or declares and records a stochastic result under the documented
   reproducibility contract.
5. **Given** an extension timeout, exception, invalid result or unavailable
   optional dependency, **When** APP invokes it, **Then** invocation fails with
   typed evidence or uses only an explicitly configured fallback; Core never
   silently accepts partial output.
6. **Given** no external implementation for one hook, **When** the decision is
   required, **Then** APP invokes the versioned default through the same Python
   port and records its identity rather than executing hidden legacy logic.
7. **Given** a new production score/rank/select/order site, **When** static
   architecture validation runs, **Then** the build fails until the site is
   classified in the decision inventory with a Python port, default,
   validator and test, or is justified as a non-overridable mechanism.
8. **Given** a request that permits several Qwen model sizes or quantizations
   subject to an explicit quality floor, **When** the selected
   `ModelVariantPolicy` runs, **Then** it proposes a bounded immutable candidate
   set containing only authorized alternatives; partition and Provider
   assignment may jointly choose one candidate from that set, and the final
   decision records model/revision/precision/quantization lineage. An exact
   operator model constraint becomes a singleton candidate and cannot be
   silently replaced.
9. **Given** one configured process-local `DistributedInferenceEngine`, **When**
   a request traverses model, deployment, partition, cache-affinity,
   assignment, target, admission, scheduling, tuning, execution and recovery
   decision epochs, **Then** each invoked decision is validated and evidenced,
   no hidden branch competes with the selected suite, and no network
   coordinator or new authority is created.

---

### User Story 3 - Execute Concurrent Request-Scoped Assignments (Priority: P3)

An application can issue concurrent requests with different deployment or
provider preferences without modifying process-global environment state and
without one request observing another request's assignment.

**Why this priority**: The current environment-variable bridge is a concrete
concurrency and ownership leak that must be removed before the separation is
credible.

**Independent Test**: Launch two overlapping requests in one process with
opposite role-to-provider assignments and verify that each request executes only
its own immutable assignment while the legacy environment variable remains
absent and unchanged.

**Acceptance Scenarios**:

1. **Given** two simultaneous requests with distinct assignment contexts,
   **When** their ACK/selection/execution intervals overlap, **Then** each request
   retains its own role binding and evidence lineage.
2. **Given** a caller using the legacy deployment helper, **When** compatibility
   mode is enabled, **Then** the helper translates to the request-scoped contract
   without mutating global environment state.
3. **Given** a missing or malformed assignment context, **When** execution is
   requested, **Then** Core uses only the documented safe default or returns an
   explicit rejection; it never silently inherits another request's state.

---

### User Story 4 - Deploy, Use, and Migrate Without Breaking Evidence (Priority: P4)

Operators can validate, resolve, apply, observe, roll back, drain and delete one
immutable distributed-inference deployment revision; application developers can
submit, reopen, observe, cancel and retrieve one durable inference request.
Maintainers can migrate the corresponding APP façades, planners, model adapters,
operations tools, examples, container images and iTiger launchers to their
declared owners while existing supported callers receive a bounded compatibility
path and historical Spec 107/109/110 evidence remains immutable.

**Why this priority**: Physical separation is useful only when it yields one
complete deployment/use workflow. That workflow must follow behavior and
distributed-consistency characterization and cannot invalidate candidate-bound
research evidence.

**Independent Test**: From clean owner-profile installations and external model
artifacts, execute validate -> resolve -> dry-run -> apply -> ready/active ->
submit -> certified result -> requester restart/reopen -> drain -> inactive,
then run the legacy import/CLI compatibility suite and show that historical
evidence is unchanged while the new execution uses a new candidate identity.

**Acceptance Scenarios**:

1. **Given** a currently supported top-level import or command, **When** it is
   used during the compatibility window, **Then** it resolves to the new owner
   or emits the documented deprecation without changing behavior.
2. **Given** a Core-only installation, **When** dependency/module inventories
   and static OCI profile inputs are inspected, **Then** APP, GUI, experiment,
   and optional model implementations are absent without building an image.
3. **Given** a Spec 110 or later iTiger run after implementation, **When** its
   candidate is materialized, **Then** it has a new immutable source/release/image
   identity and passes the required offline gates before submission.
4. **Given** a migration phase that fails its gate, **When** rollback is invoked,
   **Then** the preceding compatibility release remains runnable without data,
   authority, or evidence migration.
5. **Given** an operator deployment definition with external model references,
   **When** it is validated and resolved, **Then** one immutable revision digest
   binds service, security, model/tokenizer/artifacts, adapters, policies,
   resource constraints and compatibility without embedding weights or secrets.
6. **Given** a resolved revision, **When** APPDeployment performs dry-run and
   apply, **Then** dry-run has zero Provider side effects and apply returns a
   durable idempotent operation handle whose readiness requires every mandatory
   role, artifact, permission, adapter and Provider revision condition.
7. **Given** APPDeployment crashes during apply, warming, activation, rollback or
   drain, **When** it restarts with the same state root and identity, **Then** it
   reopens the journaled operation and reconciles authenticated Provider evidence
   rather than assuming success or starting an unrelated action.
8. **Given** an active revision, **When** APPClient submits an inference and its
   process restarts, **Then** the durable request handle reopens the exact
   request/attempt/certificate/result rendezvous; synchronous and process-local
   Future APIs remain adapters over the same request identity.
9. **Given** a revision upgrade or rollback, **When** old attempts remain active,
   **Then** each attempt remains bound to its original revision, new assignment
   uses only an eligible ready revision, and rollback creates a new lifecycle
   epoch rather than rewriting history.
10. **Given** graceful shutdown, **When** the operator drains and deletes a
    revision, **Then** new admission stops, active work is bounded by the drain
    deadline, deployment-owned pins/state are released, shared external weights
    remain unless explicitly owned for deletion, and terminal evidence is
    flushed before APP processes stop.

### Edge Cases

- A placement policy returns a provider that disappeared after telemetry was
  sampled; Core revalidates at application time and fails or replans within the
  existing bounded attempt/deadline rules.
- A model adapter is missing, incompatible, or imports unavailable optional
  dependencies; Core remains importable and reports the adapter error only when
  that adapter is requested.
- A legacy caller imports a symbol that moved more than once during extraction;
  the compatibility manifest has one canonical target and forbids re-export
  loops.
- Two policies have the same name but different versions or digests; decision
  evidence distinguishes them and plan reuse checks treat them as different.
- A policy attempts to weaken NAC-ABE, token, lease, replay, provider-permission,
  telemetry-freshness, or attempt-epoch checks; Core rejects the proposal.
- An installed package exposes a matching entry-point name but its distribution
  name, version or digest is not allowlisted; APP refuses to import or execute
  it and Core remains importable.
- A stochastic optimizer omits its seed or cannot reproduce its decision
  digest; the result may be inspected experimentally but cannot become a
  deployable/citable candidate.
- A scheduling policy selects work whose dependencies are not ready, a cache
  policy selects an ineligible object, an admission policy relaxes a mandatory
  safety floor, or recovery exceeds the attempt/deadline budget; Core/provider
  mechanism rejects the proposal.
- An external Python extension blocks past its decision budget or raises; APP
  cancels/isolates the invocation, records typed failure evidence, and applies
  no hidden fallback.
- An execution-target policy proposes an unavailable or incompatible target, an
  execution-tuning policy
  exceeds its bounded range, or a deployment policy violates an exact operator
  constraint; the owning mechanism rejects it with typed evidence.
- An external package implements only one of the ten policies; every other
  policy resolves to its explicitly named default, with no mixed hidden path.
- A model-variant policy is configured but the request supplies one exact model
  identity; the engine preserves the exact constraint, records that the policy
  was not applicable, and does not reinterpret execution-target selection as
  model selection.
- A model-variant policy returns a model, revision, precision, quantization,
  tokenizer or draft-model combination outside the allowed alternative set;
  APP rejects it before deployment or partitioning.
- A deployment policy proposes scale-in, unload or eviction while a model
  replica has an active lease/session; the lifecycle validator rejects or
  defers the action without invalidating live execution.
- A scheduling policy forms a continuous batch containing incompatible model,
  adapter, adapter-declared batch-capability, deadline or state identities; the
  provider validator rejects the batch. A batch need not share one token phase
  when the selected adapter explicitly supports mixed prefill/decode in-flight
  batching. Proactive hedging may use only assignment-authorized replicas and
  remains scheduling, while a transition after a recorded failure remains
  recovery.
- An execution-tuning policy emits an undeclared key, changes dispatch order or
  exceeds a typed range for segment, microbatch, transfer chunk, prefetch,
  compression, overlap or speculative-decoding parameters; the proposal fails
  closed.
- A cache policy uses the wrong decision phase or attempts a non-atomic
  lookup/place/admit/retain/evict/migrate action; Core/provider state validation
  rejects it and no final compute assignment is inferred from cache affinity.
- An engine snapshot combines telemetry without per-source epoch/freshness
  lineage, violates a declared dependency-consistency rule, or contains stale
  model/cache/queue/GPU/runtime/network facts; the engine invalidates the
  affected decision and takes only a bounded re-decision path.
- An objective compares latency, cost, energy or quality values without units,
  direction, aggregation and normalization semantics, or consumes an estimate
  without horizon/confidence/source/freshness; the policy input is invalid.
- A model/plan/provider candidate expansion exceeds the decision work budget;
  the engine applies deterministic bounded pruning with recorded lineage rather
  than exhausting memory/time or silently dropping candidates.
- Telemetry changes between planning and execution-intent commit; the engine
  aborts the prepared intent, releases reservations and performs only a bounded
  re-decision. It never commits a torn model/plan/assignment/target combination.
- A scale-in or unload proposal oscillates across adjacent control epochs,
  targets a warming/draining replica or lacks an idempotency key; the deployment
  validator applies declared cooldown/residency/drain rules or rejects it.
- A streaming attempt fails after externally visible output; recovery may only
  resume from a Core-approved checkpoint/commit boundary and duplicate or stale
  output epochs are rejected.
- A tuning or model policy attempts to change sampling/decoding semantics that
  the request marked exact; the engine rejects it unless APP supplied an
  explicit authorized alternative set for those semantics.
- An external optimizer attempts to inspect prompt text, tensor payloads,
  credentials or decrypted policy material although only workload shape is
  required; least-input projection omits them and the contract test fails.
- An outcome observer fails, blocks or mutates state needed by an in-flight
  request; the current inference result remains unchanged, observer failure is
  separately evidenced, and no synchronous retry is inserted into the hot path.
- A recovery policy returns `REASSIGN`, `REPARTITION` or `REDEPLOY` together
  with a privately computed replacement; APP rejects the embedded replacement
  and delegates the new decision to the owning assignment, partition or
  deployment policy.
- A cache policy emits a preferred compute Provider rather than cache affinity
  or cache-object placement; APP treats it only as an advisory affinity input
  and requires `ProviderAssignmentPolicy` to make the final compute assignment.
- A registered Runner adapter is not selected by the active execution target;
  registration alone cannot cause construction or execution.
- A developer adds a new `min`, `max`, sort, score, rank or choice in production
  DI code; the decision inventory gate requires classification and prevents an
  unreviewed optimization branch from bypassing the Python surface.
- A process mixes new request-scoped callers and legacy callers; both resolve to
  isolated per-request assignment contexts.
- A package rollback encounters plans or configuration written by the separated
  version; persisted contracts remain readable or the migration gate blocks
  release before deletion.
- An existing iTiger job or evidence directory references the pre-separation
  candidate; it is never relabeled as evidence for the new candidate.
- A Provider prepares or commits a lease but the requester fails before a
  complete execution certificate exists; the lease remains non-executable and
  Provider-owned cleanup expires it without relying on a remote abort.
- One Provider commits while another rejects, restarts or returns a receipt for
  a different boot epoch; the coordinator cannot certify the intent and no
  partial terminal result becomes visible.
- The request coordinator crashes after certificate publication; certified
  Providers remain bounded by their execution deadline, and the restarted same-
  identity requester retrieves the exact result/evidence or advances the
  attempt epoch rather than accepting ambiguous output.
- Two coordinators for one request present different attempt epochs; Providers
  reject the lower epoch and all Data/result consumers enforce the currently
  authoritative attempt.
- A network partition occurs before commit, after certificate publication or
  during dependency transfer; no stale snapshot or unreachable coordinator may
  create/extend authority, while already certified work continues only within
  its lease, dependency and deadline bounds.
- Two lifecycle writers present different actions at the same deployment epoch;
  owner, expected-state and action-digest fencing reject the conflict. A
  destructive partial prepare cannot unload active resources.
- A Provider receives no further traffic after an owner/requester crash;
  periodic Provider-local cleanup still releases expired reservations, waiters,
  temporary artifacts, model/cache pins and subscriptions without disturbing
  another active certified binding.
- A configuration path, environment default or artifact alias resolves
  differently between validate and apply; revision resolution detects the digest
  change and requires a new revision rather than mutating the validated one.
- The runtime state root is missing, ephemeral, read-only, corrupt, locked by a
  conflicting writer or over quota; new lifecycle/request authority fails closed
  while verifiable read-only status remains available.
- A Provider is live but serves the wrong deployment revision, stale readiness,
  incomplete roles or an unverified model digest; it is excluded from READY and
  assignment rather than treated as healthy.
- `deploy_plan()` is called by a legacy client; it prepares reusable static
  metadata only and cannot be used as evidence that Providers loaded or activated
  the deployment.
- A client disconnects or its process-local Future is cancelled after
  certification; distributed execution follows the durable request handle and
  explicit cancel contract rather than treating disconnect as implicit abort.
- An upgrade becomes ready while old certified attempts run; new requests bind
  the new active revision while old attempts, outputs and cleanup stay bound to
  their original revision.
- Delete encounters a shared external weight, active lease/session, incomplete
  drain or unflushed terminal evidence; it preserves the object/state and reports
  the blocking owner instead of forcing removal.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The feature MUST define and enforce an ownership matrix separating
  NDNSF Core facts/mechanisms, NDNSF-DI Core execution, application/planner
  policy, model adapters, operations tooling, and experiments.
- **FR-002**: NDNSF-DI Core MUST expose workload-neutral contracts for execution
  plans, provider assignments, dependency execution, runtime state, leases,
  bounded recovery, cache/state binding, and execution evidence.
- **FR-003**: NDNSF-DI Core MUST NOT import APP façades, application deployment
  policy, model partition algorithms, model-family implementations, GUI code,
  experiment harnesses, or operator CLI implementations.
- **FR-004**: Application and planner layers MUST depend on Core contracts; Core
  MUST NOT depend on those layers.
- **FR-005**: Model partition candidates, split objectives, deployment choice,
  optional quality-constrained model-variant choice, ordinary and multi-role
  Provider assignment, scheduling/dispatch, bounded execution tuning, cache
  objectives, admission preferences, execution-target choice, SLO weights and
  workload recovery preferences MUST be owned outside Core and exposed through
  Python policy ports.
- **FR-006**: Core MUST retain final authority over eligibility, telemetry
  freshness, fragment compatibility, resource feasibility, lease validity,
  assignment validation, attempt epochs, stale-result rejection, and bounded
  recovery.
- **FR-007**: `ProviderAssignmentPolicy` MUST accept immutable plan candidates
  and an eligible candidate snapshot and return a decision containing policy
  identity/version, selected plan identity, ordinary or multi-role assignments,
  requested multiplicity, rejected alternatives, score explanation and input
  lineage.
- **FR-008**: At least two policy implementations MUST exercise the unified
  Provider assignment interface for both its one-role and multi-role shapes: a
  fixed deterministic assignment and a configurable cost-based assignment.
- **FR-009**: Model-specific planners and runners MUST connect through explicit
  planner and runner adapter contracts; unavailable adapters MUST not prevent a
  Core-only import or validation run.
- **FR-010**: Per-request deployment and role-provider preferences MUST travel in
  an immutable assignment context and MUST NOT be transported through mutable
  process-global environment variables.
- **FR-011**: Concurrent requests MUST have isolated assignment, policy,
  deadline, attempt, and evidence lineage.
- **FR-012**: Existing NDNSF security semantics, V2 names, NAC-ABE routing,
  permission discovery, one-time UserToken/ProviderToken checks, replay
  protection, Targeted bootstrap, and provider permission checks MUST remain
  unchanged.
- **FR-013**: The extraction MUST preserve existing native plan, provider
  runtime, dependency I/O, cache binding, Qwen generation-session, and recovery
  behavior unless a separately approved requirement explicitly changes it.
- **FR-014**: Before moving or deleting implementations, the feature MUST record
  a machine-checkable compatibility manifest covering public imports, commands,
  schemas, configuration keys, C++ targets/headers, examples, tests, packaging,
  and experiment callers.
- **FR-015**: Compatibility adapters MUST have an owner, observability,
  deletion criteria, and a bounded end condition; a compatibility adapter MUST
  NOT duplicate authority or business logic.
- **FR-016**: The feature MUST provide independently installable or verifiably
  isolated profiles for Core, APP/planner, optional model adapters, and
  operations tooling, with a compatibility aggregate for the transition.
- **FR-017**: A Core-only profile MUST exclude GUI, experiments, APP façades,
  planner implementations, and optional ONNX/Qwen/llama.cpp implementations.
- **FR-018**: Existing supported imports and commands MUST remain behaviorally
  compatible during the documented migration window or fail with a specific
  migration instruction.
- **FR-019**: Every migration phase MUST have a non-destructive rollback path and
  MUST complete characterization tests before implementation deletion.
- **FR-020**: Historical Spec 107/109/110 evidence and candidate identities MUST
  remain immutable; the separated implementation MUST receive a new candidate
  identity and MUST NOT inherit prior execution claims.
- **FR-021**: A post-separation iTiger candidate MUST pass all applicable offline
  source, runtime, security, container, SIF, model, route, and evidence gates
  before any Slurm submission is authorized.
- **FR-022**: English and Chinese architecture, installation, migration, and APP
  development documentation MUST remain semantically synchronized.
- **FR-023**: The feature MUST include negative tests for forbidden dependency
  directions, optional-dependency import leakage, global environment mutation,
  concurrent assignment bleed, policy bypass, stale telemetry, invalid leases,
  malformed compatibility mappings, and candidate-evidence relabeling.
- **FR-024**: Performance validation MUST use matched topology, workload,
  duration, warmup, logging, timeout, and candidate identity; negative or neutral
  results MUST be retained as measured outcomes.
- **FR-025**: The feature MUST not introduce new NDNSF wire names, service
  authority, planner service, coordinator, persistence engine, or network
  protocol solely to achieve source/package separation.
- **FR-026**: Core MUST own the minimal workload-neutral decision-port request,
  result and validation contracts needed by execution. A public optimization
  SDK MUST re-export those contracts and define one-method Python interfaces for
  ten policies: `DeploymentPolicy`, `ModelVariantPolicy`, `PartitionPlanner`,
  `ProviderAssignmentPolicy`, `SchedulingPolicy`, `ExecutionTuningPolicy`,
  `CachePolicy`, `AdmissionPolicy`, `RecoveryPolicy` and
  `ExecutionTargetPolicy`. The SDK MUST also expose `RunnerAdapter` and
  `ExecutionAdapterRegistry` as an adapter/factory SPI outside
  `OptimizationSuite`; Core MUST NOT import the SDK.
- **FR-027**: APP MUST accept an instance-scoped `OptimizationSuite` through its
  public constructor/configuration path; Core MUST NOT import the suite,
  discover plugins, or hold a process-global optimization registry.
- **FR-028**: External Python extensions MUST be usable as standalone wheels
  without editing NDNSF-DI source or importing private modules. Explicit object
  registration MUST work without package metadata; optional entry-point
  discovery MUST be opt-in and restricted by distribution name, version and
  digest allowlists.
- **FR-029**: Every optimization invocation MUST record extension identity,
  contract version, input/configuration digest, random seed, decision budget,
  output digest, duration, status and typed failure/fallback evidence.
- **FR-030**: Core/provider mechanisms MUST validate all external decisions and
  retain final authority over plan consistency, eligibility, leases,
  dependency readiness, concurrency/deadline bounds, cache validity/security,
  mandatory admission floors, recovery bounds and result authority.
- **FR-031**: Extension timeout, exception, cancellation, malformed output,
  version mismatch and missing dependency MUST fail closed. A fallback MUST be
  explicitly named/configured and visible in decision evidence; implicit
  fallback is forbidden.
- **FR-032**: The repository MUST provide an external contract-test kit,
  standalone example optimizer package and out-of-tree native runner sample
  that use only public installed headers/modules and require no repository path
  injection.
- **FR-033**: The separation MUST produce independently buildable/installable
  Core, optimization SDK, APP/planner and model-adapter distribution artifacts;
  an aggregate compatibility distribution MAY remain only as a bounded mapping.
- **FR-034**: Extension execution MUST be owned by the application layer at the
  invocation site: APPDeployment for deployment and deployment-time execution
  target decisions; APPClient for partition/unified Provider assignment and
  request-time execution target decisions; APPProvider for scheduling,
  execution tuning, cache, admission, provider execution-target decisions and
  adapter creation. Recovery MUST reuse the executor associated with the failed
  path; Core MUST own neither plugin processes nor plugin lifecycle.
- **FR-035**: Package allowlisting and worker-process execution MUST be described
  as identity verification and fault/resource containment, not as a security
  sandbox. Operators MUST explicitly trust extension code, and extensions MUST
  receive only the data required by their declared hook.
- **FR-036**: A machine-readable decision-point inventory MUST classify every
  production NDNSF-DI choice, score, rank, ordering, planning and bounded-tuning
  site with source symbols/callers, Python port, invocation owner, validator,
  default identity and evidence. Static validation MUST fail on an unclassified
  policy-bearing decision point.
- **FR-037**: Every current built-in optimization behavior MUST migrate to a
  versioned implementation of the same public Python port available to external
  packages and compose into `DefaultOptimizationSuite`. No internal/default
  implementation may use a privileged bypass, private proposal schema or hidden
  fallback path.
- **FR-038**: All ten policy ports plus independent Runner adapter and optional
  `OptimizationObserver` SPIs MUST be implementable and invocable from Python.
  A C++ mechanism that applies a
  selection MUST consume a validated Python-produced proposal or an exact
  operator constraint; it MUST NOT retain a competing native-only optimization
  decision. Native runners remain allowed as registered execution adapters
  proposed/configured through `ExecutionTargetPolicy`, selected with
  `ProviderAssignmentPolicy` and created through `ExecutionAdapterRegistry`.
- **FR-039**: Ordinary Provider selection and multi-role placement MUST share
  one `ProviderAssignmentPolicy`; an ordinary request is represented as one
  synthetic role with requested multiplicity, and both shapes use the same
  candidate/assignment lineage and Core validation.
- **FR-040**: `SchedulingPolicy` MUST atomically propose ready-work dispatch
  order plus bounded worker concurrency and queue-window grants.
  `ExecutionTuningPolicy` MUST be limited to per-invocation tuning such as
  segment size, microbatch and compression and MUST NOT independently dispatch
  work or change queue/worker capacity.
- **FR-041**: `CachePolicy` MAY choose cache reuse, admission, cache-object
  placement, prefetch, retention and eviction and emit cache affinity, but MUST
  NOT make the final compute Provider assignment. Final compute assignment MUST
  be made by `ProviderAssignmentPolicy` over cache facts and all other eligible
  constraints.
- **FR-042**: `RecoveryPolicy` MUST return only a Core-allowed transition
  directive. Reassignment, repartition and redeployment MUST invoke
  `ProviderAssignmentPolicy`, `PartitionPlanner` and `DeploymentPolicy`,
  respectively; recovery MUST NOT embed competing implementations of those
  decisions.
- **FR-043**: Planner implementation lookup MUST be explicit registry/config
  resolution rather than optimization. `ExecutionTargetPolicy` MUST propose
  bounded compatible registered runtime/engine/device/fallback candidates for
  each plan-role-Provider alternative; final target identities MUST be selected
  with Provider assignment and resolved only by `ExecutionAdapterRegistry`.
- **FR-044**: `ModelVariantPolicy` MUST return a bounded
  `ModelVariantCandidateSet` rather than prematurely committing one variant;
  `PartitionPlanner` MUST return immutable plan candidates bound to candidate
  variant identities; and `ProviderAssignmentPolicy` MUST select a variant
  identity, plan identity and compatible role-target identities together with
  its Provider assignments. This MUST permit joint model/partition/Provider/
  target scoring without an untyped universal callback or authority to invent
  an unauthorized variant or target.
- **FR-045**: The APP layer MUST expose an instance-scoped, process-local
  `DistributedInferenceEngine` composition root that owns the configured
  `OptimizationSuite`, `ExecutionAdapterRegistry`, decision executors and
  decision evidence flow while delegating validation/execution to Core. The
  engine MUST NOT be an NDN service, cluster-wide coordinator, new authority,
  persistence engine or process-global singleton.
- **FR-046**: The engine MUST execute an explicit bounded decision DAG rather
  than an undocumented fixed call chain. The DAG MUST define deployment/session,
  request-planning, provider-execution, cache-update and recovery epochs;
  dependencies, invalidation triggers, re-entry edges, invocation owner,
  validator and evidence for every node MUST be machine-readable.
- **FR-047**: Every applicable policy MUST consume a shared immutable
  `OptimizationObjective` that distinguishes hard constraints from weighted
  preferences and can express end-to-end deadline, TTFT, TPOT, throughput,
  quality floor/metric identity, cost, energy, fairness/priority and
  availability/recovery preferences. Exact operator constraints MUST dominate
  every weighted preference.
- **FR-048**: Policies MUST receive least-input projections from one immutable
  lineage-bound `EngineSnapshot` covering applicable Provider capabilities,
  queue/worker state, GPU memory/compute, network RTT/bandwidth/loss, model and
  deployment residency, cache/KV/prefix state, runtime/adapter compatibility,
  workload facts and telemetry freshness. A snapshot MAY contain different
  per-source sample epochs when each is lineage/freshness tagged; untracked,
  internally inconsistent or stale mixtures MUST be rejected or trigger only
  bounded re-decision.
- **FR-049**: `ModelVariantPolicy` MUST be invoked only when APP/operator input
  provides two or more semantically acceptable alternatives under an explicit
  quality contract. It MAY rank/prune model identity/family/size, revision,
  tokenizer, precision, quantization, optional draft model and adapter variants
  only from that set. With one exact model constraint, the engine MUST create a
  singleton candidate, record `NOT_APPLICABLE_EXACT_CONSTRAINT`, and MUST NOT
  call a policy to replace it.
- **FR-050**: `DeploymentPolicy` MUST be a control-plane/session-epoch policy,
  not a per-token or unconditional per-request hook. Its typed actions MUST
  cover rank/use-existing, activate, reserve, prewarm, scale-out, scale-in and
  unload/evict; active leases, sessions, artifact identity, capacity and exact
  constraints remain non-overridable.
- **FR-051**: `SchedulingPolicy` MUST own atomic batch membership and dispatch
  ordering, bounded worker concurrency and queue-window grants, including
  continuous/dynamic batching, prefill/decode phase arbitration,
  fairness/priority, preemption/resume and proactive hedged dispatch when those
  alternatives are enabled. It MUST NOT select a model, partition, Provider or
  runtime and MUST NOT convert post-failure recovery into scheduling.
- **FR-052**: `ExecutionTuningPolicy` MUST return only values from a declared
  typed `TuningParameterSpec` set with type, range, scope and compatibility
  constraints. Supported families MAY include segment/microbatch, compression,
  transfer-chunk, prefetch-depth, compute/communication overlap and
  speculative-decoding window parameters; undeclared dictionaries, work
  dispatch, capacity allocation and authority changes are forbidden.
- **FR-053**: `CachePolicy` MUST identify its decision kind and phase using a
  closed action set including lookup/reuse, place, prefetch, admit, retain,
  evict and migrate/replicate. Pre-assignment cache affinity and post-execution
  cache mutation MUST have separate epochs and atomic state validation while
  remaining one policy family rather than multiple overlapping policies.
- **FR-054**: Load balancing MUST remain part of
  `ProviderAssignmentPolicy`; model/tensor/pipeline/expert/data parallel layout
  MUST remain part of `PartitionPlanner`; replica scaling and model residency
  MUST remain part of `DeploymentPolicy`; communication parameter tuning MUST
  remain part of `ExecutionTuningPolicy`; memory ownership MUST remain split
  among deployment, cache, tuning and Core feasibility. No separate placement,
  resource-allocation, load-balancing, scaling, parallelism, communication,
  memory, speculative-execution or backend-selection policy may duplicate
  these authorities.
- **FR-055**: Large-scale policy SDK implementation MUST NOT begin until the
  engine/objective/snapshot/model-variant and refined deployment/scheduling/
  tuning/cache contracts have strict structure PASS, deterministic
  cross-artifact analysis with no unresolved CRITICAL/HIGH finding, and a
  code-aware pre-implementation audit verdict of PASS.
- **FR-056**: Every objective metric MUST declare a stable identity, unit,
  minimize/maximize direction, aggregation/quantile, normalization rule,
  missing-data rule and hard-versus-preference role. Weighted preferences MUST
  NOT combine incomparable raw values, and quality/SLO feasibility MUST be
  evaluated before preference ranking.
- **FR-057**: Predicted or sampled workload, output-length, service-time,
  memory, queue and network facts MUST use an `EstimateEnvelope` declaring
  observed/predicted kind, value or bounded distribution, confidence, horizon,
  source, sampled time, maximum age and missing-data behavior. Policies MUST be
  able to distinguish observation, forecast and unknown.
- **FR-058**: `SchedulingPolicy` MUST remain one policy family but every call
  MUST declare `REQUEST_DAG` or `PROVIDER_LOCAL` scope. APPClient/request-engine
  ownership covers cross-role/transfer readiness and assignment-authorized
  hedge dispatch; APPProvider ownership covers local queue, continuous/dynamic
  batch membership, prefill/decode arbitration, preemption and worker grants.
  A decision from one scope MUST NOT mutate the other scope.
- **FR-059**: `AdmissionPolicy` MUST remain one policy family but every call
  MUST declare `ENGINE_REQUEST` or `PROVIDER_LOCAL` scope. Engine admission may
  apply tenant/quota/SLO/backpressure constraints before expensive planning;
  provider admission may apply only local readiness/capacity safety after
  assignment. Both may be stricter than mandatory floors and neither may admit
  work rejected by the other.
- **FR-060**: The Runner adapter contract MUST advertise immutable execution and
  `BatchCapability` facts, including supported model/artifact identities,
  precision/quantization, device/compute capability, parallel layout,
  continuous/in-flight batching and KV/prefix/checkpoint features. Scheduling
  MUST use adapter-declared compatibility keys instead of a framework-wide
  assumption that every batch shares one token phase.
- **FR-061**: Before starting side-effecting execution, the engine MUST prepare
  one `ValidatedExecutionIntent` that binds objective/snapshot/policy state,
  selected model variant, plan, Provider assignment, cache affinity,
  execution target, tuning and reservation/lease preconditions. Core/provider
  mechanisms MUST revalidate and atomically commit or abort that intent;
  abort MUST release acquired reservations and MUST NOT expose partial state.
- **FR-062**: Deployment actions MUST carry idempotency identity and lifecycle
  preconditions for readiness, warming, active, draining and terminated states.
  Scale-in/unload MUST honor active leases, minimum residency, cooldown/
  hysteresis and bounded drain; repeated proposals MUST be idempotent and
  rollback/release MUST be explicit.
- **FR-063**: Request semantics such as tokenizer, prompt formatting, sampling,
  stop conditions and decoding parameters MUST be exact constraints unless APP
  supplies an explicit authorized alternative set. `ModelVariantPolicy` and
  `ExecutionTuningPolicy` MUST NOT silently trade output semantics for cost or
  speed.
- **FR-064**: The decision budget MUST bound wall time, CPU/memory/work units and
  model/plan/provider candidate cardinality. Candidate pruning, sampling or
  truncation MUST be deterministic under the replay seed, preserve exact/hard
  constraints and record rejected/pruned lineage.
- **FR-065**: Engine snapshots and policy projections MUST exclude prompt text,
  raw tensors, credentials, tokens, decrypted policy material and unrelated
  tenant data by default. A policy receives only workload shape and explicitly
  consented application metadata needed by its declared hook.
- **FR-066**: Streaming/generative execution MUST expose Core-owned progress,
  output-commit and checkpoint epochs sufficient for recovery validation.
  `RecoveryPolicy` may choose only Core-advertised restart/resume boundaries;
  mechanism code MUST reject duplicate, reordered or stale visible output.
- **FR-067**: The SDK MUST provide an optional independent
  `OptimizationObserver` SPI, outside `OptimizationSuite`, that receives a
  bounded immutable `OptimizationOutcome` after commit/completion/failure for
  offline learning, evaluation and audit. Observer callbacks MUST be off the
  current request's critical decision path, idempotent by outcome identity,
  unable to change the completed/current decision, and separately budgeted and
  evidenced.
- **FR-068**: Stateful external algorithms MAY persist state under APP/extension
  ownership, but every decision MUST record a `policy_state_epoch` and
  `policy_state_digest`; concurrent updates MUST use declared snapshot/version
  semantics. Core MUST NOT provide a plugin database, silently share mutable
  state across suite instances or require observer success for inference.
- **FR-069**: Multi-tenant cache/KV reuse and mutation MUST validate tenant/
  security scope, artifact/model/tokenizer identity, epoch, reference count and
  active execution bindings. Eviction or migration MUST be atomic with leases
  and MUST NOT expose cached content or timing-derived affinity across
  unauthorized tenants.
- **FR-070**: The independent optimization surface for this feature is exactly
  ten policy ports plus two non-policy SPIs: `RunnerAdapter` for runner creation
  and optional `OptimizationObserver` for outcome feedback. Estimators, state
  stores and transaction coordinators remain suite/APP/Core mechanisms unless a
  later interoperability requirement proves a distinct public SPI is needed.
- **FR-071**: The distributed consistency model MUST identify the initiating
  APPClient as request coordinator for one requester/request/attempt and one
  operator-authorized APPDeployment identity as the single writer for one
  deployment lifecycle stream. It MUST explicitly state that this is requester-
  coordinated distributed execution, not leaderless consensus or a cluster-
  global transaction service.
- **FR-072**: Distributed execution MUST reuse the existing generic
  `PREPARE`/`COMMIT`/`ABORT`/`RENEW`/`RELEASE` execution lease mechanism,
  authenticated Targeted lease service, V2 Selection path, Provider boot epoch
  and execution-attempt authority. It MUST NOT create a competing lease table,
  coordinator service or new top-level NDN wire name.
- **FR-073**: A multi-Provider intent MUST prepare every required lease,
  revalidate the whole intent and authenticate every committed Provider receipt
  before producing one immutable `ExecutionCommitCertificate`. Prepared or
  committed leases MUST remain non-executable until a Provider validates that
  certificate through the assignment/Selection activation boundary.
- **FR-074**: Every execution certificate MUST bind requester, request ID,
  positive attempt epoch, intent/objective/snapshot/policy/plan digests, exact
  role/Provider/target membership, Provider boot epochs, lease/receipt digests,
  deadline and result-rendezvous identity. Missing, stale, incomplete,
  mismatched or unauthenticated certificates MUST fail closed.
- **FR-075**: Request attempt epoch, Provider boot epoch and deployment lifecycle
  epoch MUST be non-interchangeable fencing dimensions. Providers MUST reject a
  stale dimension even when identity or idempotency key matches; idempotency
  MUST deduplicate only the same operation and digest. Resource cleanup MUST
  retain bounded highest-epoch tombstones through the operation/result replay
  window, and a new prepare MUST bind the expected Provider boot epoch.
- **FR-076**: Requester crash semantics MUST be phase-specific. Pre-certificate
  leases MUST abort or expire without execution; certified work MAY continue
  only to its committed deadline; a restarted same-identity requester MUST
  retrieve immutable attempt-scoped result/evidence or start a higher attempt
  epoch. Cross-identity takeover MUST require explicit authorization.
- **FR-077**: Under network partition or missing authority facts, new planning,
  lifecycle, lease, renewal and recovery actions MUST fail closed. Already
  certified work MAY continue only while local lease, security, dependency and
  deadline checks remain valid; healing MUST reject stale attempt/lifecycle/
  Provider epochs rather than merging authority.
- **FR-078**: Every prepared, committed, executing or lifecycle-prepared record
  MUST carry a finite reservation TTL or execution deadline and an idempotent
  cleanup action. Provider cleanup MUST run both on operation entry and on a
  bounded periodic timer, with declared upper bounds and evidence for release
  of orphan compute, memory, model/cache/KV pins, subscriptions, temporary
  artifacts and waiter entries.
- **FR-079**: Deployment actions MUST use an owner-bound lifecycle epoch,
  expected previous-state digest, action digest, target set, idempotency key and
  authenticated prepare/apply certificate. Duplicate actions are idempotent;
  conflicting same-epoch actions fail; destructive scale-in/drain/unload fails
  closed unless every active lease/session and residency precondition validates.
- **FR-080**: The system MUST guarantee atomic application-visible output rather
  than claiming simultaneous distributed state transition. Intermediate and
  terminal Data MUST bind request, attempt, Provider boot and certificate/
  intent digests; incomplete, duplicate, reordered, late or stale output MUST
  never become an accepted terminal result.
- **FR-081**: Authenticated Provider receipts used for a certificate MUST retain
  operation/schema, signer/certificate or equivalent proof, Data/content
  digest, Provider/request/attempt/intent/lease bindings and expiry. A wrapper
  MUST NOT promote unproven payload bytes to distributed commit evidence.
- **FR-082**: Before Engine execution-intent implementation begins, contract and
  MiniNDN fault tests MUST cover every prepare/revalidate/commit/certificate/
  activation/output/release boundary, duplicate/reordered/lost operations,
  requester and Provider restart, conflicting lifecycle writers, network
  partitions and idle orphan cleanup. The test gate MUST distinguish safety from
  availability and preserve every failed outcome.
- **FR-083**: The public APP contract MUST distinguish mutable
  `DeploymentDefinition`, immutable `DeploymentRevision`, mechanism-owned
  `DeploymentInstanceRecord`, client-only `PreparedPlanSession` and durable
  `InferenceRequestHandle`. Existing `DistributedInferenceDeployment` remains a
  definition compatibility façade; `deploy_plan()` MUST become a compatibility
  alias for metadata-only `prepare_session()` and MUST NOT imply Provider
  activation.
- **FR-084**: Validation/resolution MUST produce one canonical revision digest
  binding service/security identities, exact or authorized model semantics,
  role/dependency plan, policy/adapter/runtime versions, resource/SLO bounds,
  external artifact references and compatibility capabilities. Unknown,
  unresolved, incompatible or digest-changing input MUST fail before side
  effects.
- **FR-085**: Model/tokenizer/runtime artifacts MUST remain external and use
  identity/revision/URI-or-mount/digest/size/format/access-reference/staging/
  retention metadata. Definitions, revisions, journals and evidence MUST NOT
  embed model weights, credentials, private keys, tokens or raw request payloads;
  Providers MUST verify required artifact identity before readiness.
- **FR-086**: APPDeployment and APPClient recovery MUST use one fixed APP-owned
  versioned `RuntimeJournal` mechanism under an operator-configured persistent
  state root. The default append-only filesystem journal MUST provide canonical
  checksums, atomic/locked durable transitions, quotas, retention/compaction and
  typed corruption/lock/version/quota errors. It is neither an optimization SPI
  nor a cluster database, and journal failure MUST fail closed for new authority.
  State MUST be partitioned by canonical application/NDN owner identity and
  deployment/request stream; identity mismatch, traversal, symlink and cross-
  tenant record/spool access MUST fail before reads.
  Owner-only protected request-envelope storage MAY share the persistent root,
  but the journal MUST contain only its name/digest/security/expiry reference.
  A durable submit MUST protect and persist the existing request wire envelope
  before returning; failure MUST NOT degrade silently to process-memory recovery.
- **FR-087**: APPDeployment MUST expose semantic validate, resolve, dry-run plan,
  apply, status, wait, rollback, drain and delete operations. Every mutation MUST
  be idempotency/lifecycle fenced, journaled and return a reopenable operation
  handle; dry-run MUST create zero Provider mutation and delete MUST fail closed
  on active bindings or shared-artifact ownership.
- **FR-088**: Definition/revision state MUST distinguish DRAFT, VALIDATED and
  immutable RESOLVED; instance phase MUST distinguish ABSENT, APPLYING, STAGING,
  WARMING, READY, ACTIVE, DRAINING, INACTIVE and DELETED. Reconciling, Ready,
  Active, Degraded, Failed and Unknown MUST be reason-coded conditions over the
  phase rather than conflicting linear states. READY MUST bind required role counts, revision/Provider boot
  epochs, artifact/adapter/runtime digests, permissions, capacity and fresh
  readiness probes; liveness alone MUST NOT imply readiness or activation, and
  unverifiable partitioned state MUST become Unknown rather than INACTIVE.
- **FR-089**: The supported startup contract MUST order owner-profile install,
  persistent/artifact/trust mounts, validate/doctor, existing NFD and
  ServiceController availability, externally launched generic APPProvider agent
  registration, APPDeployment-selected revision staging/warming/activation, then
  APPClient use. APP MUST verify but MUST NOT become the hidden process supervisor
  or OS/container/job scheduler for NFD, ServiceController or Provider processes.
- **FR-090**: APPClient MUST expose durable submit/open-request/status/wait/
  result/cancel and conditional stream semantics through
  `InferenceRequestHandle`. Synchronous `infer()`/`distributed_inference()` and
  process-local Future APIs MUST delegate to the same request/attempt/
  certificate/result identity and MUST NOT be the crash-recovery authority.
  The handle MUST bind a protected `RequestEnvelopeReference`; reopen MAY retry
  only from a same-identity digest-matching retained envelope and MUST fail with
  a typed terminal reason when that evidence is missing, expired or unverifiable.
- **FR-091**: Request cancellation MUST be explicit, idempotent and attempt-
  fenced. Pre-certificate cancellation aborts/expires reservations; post-
  certificate cancellation stops new dependent work and requests bounded
  Provider cancellation without revoking an already accepted terminal result.
  Timeout or client disconnect MUST NOT imply cancellation unless declared.
- **FR-092**: APPDeployment MUST periodically reconcile desired and observed
  state from journaled action identity plus authenticated Provider evidence.
  Upgrade MUST create a new immutable revision; old attempts remain bound to
  their revision; rollback MUST apply a previous revision under a new lifecycle
  epoch rather than rewriting history.
- **FR-093**: Graceful shutdown MUST drain new admission, bound active certified
  work, preserve terminal evidence, release deployment-owned leases/sessions/
  pins/temporary artifacts, flush the journal and publish INACTIVE before APP
  processes stop. Shared external artifacts and externally supervised NFD/
  controller processes MUST not be deleted/stopped implicitly.
- **FR-094**: `ndnsf-di-ops` MUST be a thin adapter over public APP APIs and
  expose validate/resolve/plan/apply/status/wait/rollback/drain/delete/doctor/
  events/metrics plus request submit/status/wait/result/cancel/stream semantics.
  Human and JSON
  output MUST share versioned reason-coded conditions, owner, retryability,
  observed generation and evidence/journal cursor; CLI MUST contain no second
  lifecycle or inference implementation.
- **FR-095**: Providers and requests MUST bind one exact deployment revision.
  Mixed revisions are assignment-ineligible unless the revision explicitly
  declares and tests wire, model-state and dependency compatibility. Activation
  may move new requests only after the target revision is READY; certified old
  attempts continue under their original revision.
- **FR-096**: Before Spec 111 is considered operationally deployable, a clean
  owner-profile MiniNDN workflow MUST complete validate -> resolve -> dry-run ->
  apply -> ready/active -> submit -> certified result -> requester restart/open
  -> drain -> inactive with immutable candidate/revision/request evidence. This
  gate proves workflow correctness only and MUST NOT be reported as iTiger,
  production-HA or Qwen-performance evidence.
- **FR-097**: The iTiger runtime contract MUST treat Docker/OCI as an immutable
  build/distribution source and MUST execute its digest-bound SIF only inside a
  Slurm allocation with Apptainer. It MUST NOT require a Docker daemon on iTiger
  or present an allocation as a persistent public-IP service.
- **FR-098**: An operations-owned runtime adapter MUST map one exact Spec 111
  candidate and `DeploymentRevision` to a distinct
  `InfrastructureAllocationHandle`. Scheduler PENDING/RUNNING/terminal state
  MUST remain separate from APPDeployment READY/ACTIVE/INACTIVE and request
  terminal state; no scheduler state may imply inference success.
- **FR-099**: The post-Spec-111 allocation process map MUST derive Provider/role
  cardinality from the immutable revision rather than hard-code three stages.
  Every NFD/controller/deployment-coordinator/provider/client project process
  MUST run inside the same verified SIF, while the host supervisor owns only
  Slurm, Apptainer, allocation routing and evidence finalization.
- **FR-100**: iTiger binds MUST expose release/model/artifact/exact-role identity
  read-only, identity-partitioned RuntimeJournal/request-spool state read-write,
  and job-local scratch/node-run directories read-write. A broad writable
  `/project`, another role's identity/state, or a host-only project executable
  MUST be rejected.
- **FR-101**: One node-local run directory MUST be bound at the same in-container
  path to the node NFD and every local process. Each Provider MUST bind its
  Slurm-assigned GPU UUID/device set without unapproved duplicate visibility;
  `--nv`, host `nvidia-smi` or job RUNNING alone MUST NOT satisfy GPU readiness.
- **FR-102**: The in-allocation order MUST be NFD/routes/controller, generic
  Provider boot-capability registration, exact-revision APPDeployment apply and
  signed READY/ACTIVE, then APPClient request, drain/INACTIVE and zero-survivor
  teardown. APPDeployment MUST NOT provision Slurm jobs or Provider processes.
- **FR-103**: Multi-node use MUST remain disabled until an allocation-scoped
  selected-transport probe proves addresses, reachability and exact NFD faces/
  routes. No persistent face/listener, login-node daemon, firewall change or
  post-allocation service is permitted.
- **FR-104**: A post-Spec-111 iTiger candidate MUST bind new source/offline-gate,
  deployment-revision, OCI/SIF, process-map, model/artifact, identity/state and
  authorization digests. Historical Spec 109/110 jobs or a pre-Spec-111 image
  MAY prove substrate facts but MUST NOT be promoted to post-separation
  deployment evidence.
- **FR-105**: Spec 111 validation MUST use three explicit tiers: local unit,
  contract, native, package and static/offline handoff checks; MiniNDN for every
  distributed network, security, fault-injection, operational-workflow and
  performance acceptance; and zero live iTiger/container-runtime activity.
  Spec 111 MUST NOT build/publish an OCI image, materialize/run a SIF, execute
  Apptainer, call `sbatch`/`srun`, or claim remote GPU/Qwen evidence. Those
  actions belong to a new post-Spec-111 candidate under Spec 110.

### Key Entities

- **CoreExecutionPlan**: Workload-neutral roles, dependencies, artifacts,
  execution invariants, and immutable plan identity consumed by Core.
- **CandidateSnapshot**: Immutable eligible and rejected provider facts sampled
  for one planning decision, including freshness and lineage.
- **ProviderAssignmentPolicy**: Application-owned strategy over one or more plan
  candidates and an eligible Provider snapshot; ordinary selection is its
  one-role form.
- **ProviderAssignmentDecision**: Selected plan identity, proposed role
  assignments and compatible role-target identities, requested multiplicity,
  score explanation,
  rejections, policy identity, and snapshot lineage subject to Core validation.
- **AssignmentContext**: Immutable per-request assignment, policy, deadline,
  attempt, and evidence identity passed to execution.
- **ModelAdapter**: Optional model-family planner and/or runner implementation
  conforming to Core contracts without changing Core semantics.
- **OptimizationSuite**: Instance-scoped composition of zero or one extension
  per one of the ten policy seams, with explicit defaults and no global
  registration.
- **DistributedInferenceEngine**: APP-owned process-local composition root that
  executes the validated decision DAG using one suite, adapter registry,
  objective, snapshot lineage and evidence recorder without becoming a network
  coordinator.
- **OptimizationObjective**: Immutable hard constraints and weighted
  preferences shared across applicable decisions.
- **EngineSnapshot**: Immutable, freshness-bound aggregate of actionable model,
  deployment, Provider, runtime, queue, cache, GPU and network facts from which
  each policy receives a least-input projection.
- **ModelAlternativeSet**: APP/operator-authorized model/revision/tokenizer/
  precision/quantization/draft/adapter alternatives and quality contract.
- **ModelVariantCandidateSet**: Bounded authorized model alternatives with
  ranking/pruning explanation and objective/snapshot/policy/input lineage; the
  final variant is selected only with plan and Provider assignment.
- **EngineDecisionGraph**: Machine-readable decision nodes, epoch boundaries,
  dependencies, invalidation/re-entry edges, owners, validators and evidence.
- **ExecutionAdapterRegistry**: Instance-scoped registry of named Runner adapter
  factories, separate from policy composition and selected only through a
  validated execution target.
- **OptimizationExtensionDescriptor**: Name, semantic version, contract version,
  distribution/source digest, supported hooks and reproducibility properties.
- **DecisionBudget**: Immutable deadline, cancellation and resource limits for
  one extension invocation.
- **OptimizationDecisionEvidence**: Extension/input/configuration/seed/output
  lineage plus duration, status and explicit fallback identity.
- **ValidatedExecutionIntent**: Prepared, revalidated and atomically committed
  binding of all decisions and reservation preconditions for one attempt.
- **RequestCoordinatorBinding**: Requester identity, request ID and positive
  attempt epoch that define the only coordinator term authorized for one
  execution attempt.
- **AuthenticatedProviderReceipt**: Core-verifiable Provider operation result
  retaining signer/Data, Provider boot epoch, lease/resource and intent lineage.
- **ExecutionCommitCertificate**: Immutable complete committed-receipt set and
  exact assignment/intent digest required before any Provider activates work.
- **DeploymentLifecycleRecord**: Single-writer owner, lifecycle epoch, expected
  state/action digest, target receipts, certificate and rollback/cleanup state
  for one deployment action.
- **DeploymentDefinition**: Mutable operator input derived from the existing
  deployment configuration, containing service/security/model/plan/policy/
  runtime/resource and external-reference intent but no running-state claim.
- **DeploymentRevision**: Immutable schema-versioned, content-addressed resolved
  definition used by Providers, deployment actions and requests.
- **DeploymentInstanceRecord**: Desired/observed Provider-set lifecycle state for
  one exact revision, including readiness conditions and current action.
- **DeploymentOperationHandle**: Reopenable deployment/revision/action/lifecycle-
  epoch identity plus event cursor returned by every mutating APPDeployment call.
- **PreparedPlanSession**: Reusable client metadata/reference handle for one
  revision; it does not prove deployment activation or Provider readiness.
- **InferenceRequestHandle**: Durable requester/request/attempt/revision/
  certificate/rendezvous identity supporting status, wait, result, cancel,
  conditional stream and same-identity reopen.
- **RequestEnvelopeReference**: Durable name or opaque locator, wire digest,
  security context and expiry for the existing authenticated/confidentiality-
  protected RequestMessage envelope; never a raw prompt in journal/status.
- **RuntimeAllocationHandoff**: Immutable bridge binding one Spec 111 candidate/
  deployment revision to exact OCI/SIF, Slurm profile/process map, model,
  identity/state/bind, network-probe and authorization evidence.
- **InfrastructureAllocationHandle**: Operations-owned runtime adapter identity
  plus Slurm job/allocation state; distinct from deployment and request handles.
- **RuntimeJournal**: Fixed APP-owned durable mechanism for lifecycle actions,
  requester receipts/certificates, rendezvous pointers, rollback and fencing
  evidence; it is not a policy or external optimizer state store.
- **ArtifactReference**: External model/tokenizer/runtime artifact identity,
  digest, size, format, location/access reference and staging/retention contract
  without embedded weights or secrets.
- **OrphanCleanupRecord**: Provider-owned expiry, released resource bindings,
  terminal reason and evidence proving bounded cleanup after coordinator loss.
- **ResultRendezvousRecord**: Attempt/certificate/output-epoch scoped immutable
  visible-result record used by same-identity requester recovery and duplicate/
  stale-output rejection.
- **OptimizationOutcome**: Bounded measured/observed result linked to decision,
  execution and candidate lineage for optional asynchronous feedback.
- **OptimizationObserver**: Independent non-policy SPI that consumes outcomes
  without changing current-request authority or success.
- **DecisionPort**: Core-owned mechanism-facing callable contract over immutable
  facts and typed proposals; the SDK re-exports/adapts it for external authors.
- **DecisionPointInventory**: Machine-readable closure map from every current
  policy-bearing source decision to its Python port, named default, validator,
  compatibility mapping and evidence.
- **DefaultOptimizationSuite**: Versioned composition of the migrated current
  policy algorithms, using exactly the same ten public Python ports and
  evidence envelope as an external suite.
- **DefaultExecutionAdapterRegistry**: Versioned registrations of the migrated
  current Python/native runner adapters using the same adapter SPI as external
  packages.
- **CompatibilityManifest**: Canonical mapping of legacy imports, commands,
  configuration and build surfaces to new owners, including expiry and deletion
  evidence.
- **CandidateIdentity**: Immutable source, dependency, plan, model, policy and
  evidence lineage for one validation candidate. Spec 111 records explicit
  `DEFERRED_TO_SPEC110` markers for build/image/SIF fields; Spec 110 creates a
  new runtime candidate rather than filling those fields in place.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Automated dependency checks find zero forbidden imports from
  NDNSF-DI Core to APP/planner policy, model implementations, GUI, experiments,
  or operations implementations.
- **SC-002**: One Core-only installation/import validation completes with zero
  optional APP, GUI, experiment, ONNX, Qwen, or llama.cpp modules loaded.
- **SC-003**: One hundred percent of inventoried supported imports, commands,
  schemas, configuration keys, build targets, and experiment callers have an
  explicit migrated owner, compatibility mapping, or approved removal record.
- **SC-004**: Fixed and cost-based placement policies select their expected
  deterministic assignments from the same fixture while Core rejects 100% of
  infeasible, stale, unauthorized, or invalid-lease proposals in the negative
  matrix.
- **SC-005**: At least 100 overlapping paired-request repetitions complete with
  zero assignment, policy, deadline, attempt, or evidence-lineage bleed and zero
  mutation of the legacy global preference environment variable.
- **SC-006**: All required security, native runtime, dependency I/O, cache,
  recovery, APP compatibility, packaging, and static container-contract
  regression suites pass with zero unexplained failures and zero container-
  runtime invocation.
- **SC-007**: A predeclared ten-pair MiniNDN non-regression campaign, with one
  60-second measured run per frozen baseline/treatment cell and no automatic
  rerun, shows no correctness/completion regression; the median paired latency
  and throughput change and its 95% bootstrap interval stay within the declared
  5% non-regression margin. Crossing the margin blocks deletion and triggers
  rollback rather than result suppression.
- **SC-008**: Every compatibility adapter reports use, has one canonical target,
  and can be deleted only after two consecutive validation snapshots observe
  zero in-repository callers and all external migration gates are satisfied.
- **SC-009**: Historical Spec 107/109/110 evidence digests remain unchanged, and
  every post-separation run records a distinct candidate identity before it can
  be cited as evidence.
- **SC-010**: English and Chinese documentation checks report zero missing or
  contradictory Core/APP ownership, installation, migration, and invocation
  instructions.
- **SC-011**: A standalone external optimizer wheel installs in a clean
  environment, imports only public SDK modules, requires zero NDNSF-DI source
  changes/path injection, implements all ten policies with no more than one
  method per policy, independently registers one Runner adapter, and passes
  100% of the public policy and adapter contract-test kit.
- **SC-012**: The external fixture's deployment, partition, unified Provider
  assignment, scheduling, execution tuning, cache, admission, recovery,
  execution-target and Runner adapter paths complete their focused integration
  paths including one MiniNDN
  inference path, while 100% of malicious/stale/over-budget fixture decisions
  are rejected with typed evidence and no security/authority bypass.
- **SC-013**: At least 100 concurrent invocations using two independently
  configured optimizer suites show zero registry, configuration, seed,
  assignment or evidence-lineage bleed.
- **SC-014**: Core, optimization SDK, APP/planner and each selected model adapter
  build as separate installable artifacts; clean Core and SDK inventories contain
  zero forbidden implementation modules, and an out-of-tree native runner sample
  compiles/links without editing the repository.
- **SC-015**: Static and clean-install dependency checks prove Core has zero SDK
  imports, each wheel owns a disjoint declared file set, only the compatibility
  wheel owns the legacy root `__init__.py`/modules, and uninstalling any optional
  owner wheel does not remove files owned by another wheel.
- **SC-016**: The decision-point inventory reports 100% classified production
  selection/optimization sites, zero unmapped policy-bearing sites and zero
  native-only competing decisions; mutation fixtures prove every one of the
  ten Python policies can change its intended valid outcome and that Runner
  creation changes only when the selected adapter identity changes.
- **SC-017**: Frozen before/after fixtures show the named defaults reproduce all
  characterized deterministic decisions exactly; stochastic defaults preserve
  the characterized admissible-set/multiplicity/distribution contract and add
  supplied-seed replay. A partial external suite records named defaults for 100%
  of omitted ports with zero hidden legacy/default execution.
- **SC-018**: One machine-readable engine decision graph covers 100% of the ten
  policy nodes and all deployment/session, request, provider, cache-update and
  recovery epochs; contract tests execute every allowed edge and reject every
  undeclared, stale or cyclic transition.
- **SC-019**: Objective round-trip and mutation tests cover every standardized
  hard constraint/preference field, and 100% of attempts to override an exact
  constraint or combine incompatible snapshot epochs are rejected with typed
  evidence.
- **SC-020**: Model-variant tests select expected alternatives across at least
  three model sizes and two quantization/precision profiles, preserve exact
  model constraints, reject 100% of alternatives outside the authorized set,
  and record model/tokenizer/artifact/policy lineage.
- **SC-021**: Deployment contract tests exercise use-existing, activate,
  reserve, prewarm, scale-out, scale-in and unload/evict and reject 100% of
  lifecycle actions conflicting with active leases, sessions, identity,
  capacity or exact constraints.
- **SC-022**: Scheduling tests demonstrate compatible dynamic-batch formation,
  prefill/decode arbitration, fairness, bounded preemption/resume and optional
  proactive hedging while rejecting 100% of incompatible, non-ready,
  over-capacity or post-failure-as-scheduling proposals.
- **SC-023**: Tuning mutation tests accept every declared typed parameter at
  valid boundaries and reject 100% of undeclared, wrong-type, out-of-range,
  wrong-scope, incompatible or dispatch/capacity-changing results.
- **SC-024**: Cache tests exercise every closed action kind in its allowed epoch,
  prove atomic state/lineage validation, and show zero cases where cache
  affinity directly becomes final compute assignment.
- **SC-025**: Joint-decision fixtures show at least two cases where the best
  model variant or Provider depends on partition/target feasibility; the engine
  selects the expected authorized variant/plan/Provider/target tuple and rejects
  100% of premature, unauthorized or torn tuples.
- **SC-026**: Objective/estimate tests reject 100% of missing-unit,
  wrong-direction, unnormalized, stale-horizon and incompatible-aggregation
  inputs, while deterministic candidate-budget tests reproduce the same pruned
  set and digest for the same seed.
- **SC-027**: Scheduling and admission contract tests exercise both declared
  scopes, reject 100% of cross-scope mutations, and demonstrate an
  adapter-declared mixed-phase batch plus an adapter that requires
  phase-homogeneous batches.
- **SC-028**: Execution-intent fault injection at every prepare/revalidate/
  reserve/commit/certificate/activation boundary yields either one certified
  visible attempt or no executable attempt, zero partial assignment/target/
  cache state and bounded release/expiry plus re-decision evidence.
- **SC-029**: Deployment lifecycle tests show idempotent repeated actions,
  cooldown/hysteresis, warm/readiness and drain behavior, and reject 100% of
  unsafe scale-in/unload attempts during active bindings.
- **SC-030**: Streaming recovery tests inject failure before and after output
  commit/checkpoint boundaries and observe zero duplicate, reordered or stale
  visible outputs across resume/restart/reassign paths.
- **SC-031**: An external fixture with persistent learning state receives 100%
  of unique outcome identities exactly once logically despite duplicate
  delivery, records state epoch/digest on the next decision, and demonstrates
  that observer timeout/failure changes neither current inference result nor
  Core availability.
- **SC-032**: Least-input/privacy tests expose only declared workload shape to
  all ten policies and the observer, with zero prompt/tensor/credential/token/
  decrypted-policy or cross-tenant cache disclosure.
- **SC-033**: Fault injection before and after every prepare, revalidation,
  commit, certificate and activation boundary produces zero uncertified
  executions and zero partial visible terminal results; every prepared or
  committed-but-uncertified reservation is remotely released or locally expires
  within its declared upper bound.
- **SC-034**: Duplicate, reordered, delayed and lost lease/certificate operations
  across at least 100 deterministic trials produce one idempotent result per
  operation digest, reject 100% of conflicting reuses and leave zero active
  orphan conflict keys after expiry.
- **SC-035**: Requester crash tests at pre-prepare, partial-prepare, partial-
  commit, post-certificate, streaming and post-result boundaries observe zero
  stale/duplicate visible output; same-identity restart either retrieves the
  exact certified result or advances attempt epoch with complete evidence.
- **SC-036**: Two APPDeployment processes replaying the same action and proposing
  conflicting actions at the same/adjacent lifecycle epochs produce exactly one
  authoritative action digest per epoch, zero unsafe destructive partial apply
  and complete reconciliation/rollback evidence.
- **SC-037**: Network-partition tests before commit, after certificate and during
  dependency transfer reject 100% of new stale-authority operations, never
  extend a lease without its coordinator, and retain safety while explicitly
  recording any expected availability loss.
- **SC-038**: Provider restart between receipt, certificate and activation
  invalidates 100% of old-boot receipts/leases and no old-epoch work or output is
  accepted after the new Provider epoch appears; stale operations replayed after
  resource cleanup are rejected while the bounded fencing tombstone is live.
- **SC-039**: With no subsequent request traffic, periodic Provider cleanup
  releases 100% of expired test reservations, waiters, temporary artifacts and
  model/cache/KV references within the predeclared bound while preserving all
  independently active certified bindings.
- **SC-040**: One MiniNDN distributed-consistency smoke executes the complete
  authenticated lease/certificate/activation path and injects requester loss,
  one Provider restart and one network cut with zero uncertified execution,
  zero stale visible output and complete reason-coded evidence.
- **SC-041**: Clean-install validation resolves the same deployment input twice
  to one byte-identical revision digest, rejects 100% of unresolved/incompatible/
  digest-changing inputs and records zero embedded model-weight or secret bytes.
- **SC-042**: Dry-run produces zero Provider state changes, while at least 100
  duplicate apply/reconcile calls and APPDeployment restarts injected before/
  after every lifecycle transition converge to one action digest and one
  authoritative revision state.
- **SC-043**: Readiness tests reject 100% of Providers with missing roles, stale
  probes, wrong revision/boot epoch, invalid permissions, missing/bad artifacts
  or incompatible adapters and activate only after every declared minimum ready
  condition is satisfied.
- **SC-044**: Submit, synchronous convenience, process-local Future and reopened
  request handles use one request/attempt/certificate identity; requester restart
  at every request phase yields at most one visible result, and cancellation
  tests reject 100% of stale/conflicting attempts. Missing/expired/tampered
  request-envelope references yield typed failures and never empty/rebuilt input.
- **SC-045**: Journal tests inject corruption, partial write, unsupported schema,
  lock contention, quota exhaustion, compaction and ephemeral/read-only state-
  root configuration; 100% of unsafe cases block new authority with typed reason
  while valid records reopen and preserve authoritative digests; owner-permission
  identity/traversal/cross-tenant isolation and protected request-spool retention/
  cleanup tests expose no plaintext input.
- **SC-046**: Upgrade tests keep 100% of existing certified attempts bound to the
  old revision, route new work only after the new revision becomes ready and
  demonstrate rollback as a new lifecycle epoch with unchanged historical
  revision/certificate bytes.
- **SC-047**: Drain/shutdown tests reject all new work after DRAINING, complete or
  cancel/expire every active test attempt within the predeclared deadline,
  preserve terminal evidence/shared external artifacts and leave zero
  deployment-owned lease/session/pin/temp-resource leak.
- **SC-048**: Every Python APPDeployment/APPClient lifecycle/request operation
  has one CLI equivalent whose JSON output matches deployment/revision/request/
  reason/evidence identities exactly; static checks find zero CLI-owned lifecycle
  or inference decision implementation.
- **SC-049**: One clean-profile MiniNDN workflow executes validate -> resolve ->
  dry-run -> apply -> ready/active -> submit -> certified result -> requester
  restart/open -> drain -> inactive with zero hidden repository-path dependency,
  zero uncertified execution and complete immutable evidence.
- **SC-050**: API compatibility tests prove `deploy_plan()` and
  `DeploymentSession` delegate only to `prepare_session()` and
  `PreparedPlanSession`, never publish READY/ACTIVE state, mutate Providers or
  satisfy deployment-operation evidence.
- **SC-051**: Offline handoff validation rejects every mutable-tag-only,
  pre-Spec-111, wrong-revision, wrong-SIF or mismatched process-map/model/
  identity/state candidate before render or submission.
- **SC-052**: Scheduler-state tests prove 100% of PENDING/RUNNING/PREEMPTED/
  TIMEOUT/CANCELLED/COMPLETED fixtures remain distinguishable from APP
  READY/ACTIVE/INACTIVE and request COMPLETED/FAILED/CANCELLED.
- **SC-053**: Bind tests reject broad writable project mounts, writable models/
  identities/artifacts, missing identity-partitioned `/state`, missing shared
  node-run bind, cross-role state and host-only project executables.
- **SC-054**: Process-map tests derive exact revision role cardinality, execute
  every project command through the pinned SIF runner, map each Provider to its
  assigned GPU UUID set and reject every undeclared duplicate.
- **SC-055**: Offline lifecycle tests enforce allocation preflight -> NFD/routes/
  controller -> generic Providers -> apply -> READY/ACTIVE -> request -> drain/
  INACTIVE -> zero-survivor order with no later phase on partial readiness.
- **SC-056**: A post-Spec-111 single-node iTiger acceptance run, when separately
  authorized by Spec 110, uses one new immutable candidate and records actual
  CUDA/ONNX GPU, APP apply/submit/drain and certified-result evidence without
  inheriting historical performance claims.
- **SC-057**: No multi-node candidate is render-eligible without the exact PASS
  selected-transport probe digest, one-NFD-per-node process map and bounded
  teardown contract.
- **SC-058**: Static/runtime scans find zero Docker-daemon dependency on iTiger,
  zero model weights in OCI/SIF and zero claim that a bounded allocation is an
  always-on public service.
- **SC-059**: Spec 111 closeout records all distributed acceptance evidence from
  MiniNDN, passes local package/static handoff checks without starting a
  container runtime, and contains zero new OCI/SIF digest, registry publication,
  Apptainer execution, Slurm job ID or iTiger GPU/Qwen result.

## Assumptions

- Spec 111 is an independent architecture migration and does not reopen or
  rewrite Spec 110 experimental results.
- The existing native C++ execution path is the behavioral reference and will
  be reused rather than rewritten.
- A compatibility aggregate may remain during migration, but feature completion
  requires separate installable Core, SDK, APP/planner and selected model-adapter
  artifacts; final compatibility deletion still requires its explicit exit gate.
- Planner decisions are advisory until Core validates and applies them; they do
  not become a new authority or network service.
- Distributed execution uses a request-scoped coordinator and deployment uses
  an explicit single-writer owner; Spec 111 does not promise leaderless
  consensus, cross-identity automatic takeover or availability during a network
  partition.
- Existing generic execution leases, Provider boot epochs and attempt authority
  are the canonical consistency primitives. Best-effort abort/release accelerates
  cleanup, but finite Provider-owned TTL/deadline cleanup is the safety basis.
- `DistributedInferenceDeployment` configuration is the compatibility source for
  one `DeploymentDefinition`; Spec 111 does not introduce a second independent
  deployment manifest or treat `DeploymentSession` as running deployment state.
- Operators provide a persistent writable state root to APPDeployment/APPClient
  in deployed environments. A mounted filesystem journal is the default fixed
  mechanism; a distributed database or public state-store SPI is not assumed.
- NFD and ServiceController are externally supervised prerequisites. APP/ops may
  diagnose their availability but do not silently own their process lifecycle.
- For the later Spec 110 iTiger candidate, Docker/OCI is a build source and
  Apptainer SIF is the execution format. Spec 111 defines this conceptual
  handoff only; it does not build or run either format. In the later execution,
  Slurm/Apptainer adapters launch bounded allocation processes and APP owns
  revision/model lifecycle only after generic Provider agents are live.
- “All selection and optimization APIs” means the normative policy-bearing
  decision rule, ten-policy surface and the independent Runner adapter plus
  optional `OptimizationObserver` SPIs in
  `contracts/python-optimization-surface.md`; invariant enforcement and exact
  operator constraints are intentionally not replaceable optimization policy.
- Model-variant optimization is optional per request: the public port exists in
  every suite, but the engine records it as not applicable when the operator
  provides one exact model identity.
- Model weights remain external artifacts and are never bundled into Core or
  compatibility packages.
- No physical UAV, production deployment, or new iTiger performance claim is
  required to prove source/package separation; any later iTiger execution is a
  new candidate-bound validation activity.
- Existing unrelated worktree changes and stale GSD worktrees are outside this
  feature and must not be cleaned or rewritten by Spec 111.

## Out of Scope

- Designing a new optimization algorithm or claiming that one policy is faster
  than another; Spec 111 supplies extension seams, reference policies and
  validation, not a new research algorithm.
- A stable cross-compiler C++ shared-library ABI or arbitrary runtime `dlopen`.
  Native adapters are built out of tree against public headers and linked into
  the provider executable; Python extensions use the wheel/entry-point path.
- Adding new top-level NDNSF wire names, changing security authority, NFD
  routing, NAC-ABE, permission or token semantics, or replacing the existing
  Targeted execution-lease mechanism. Versioned lease/Selection/result payload
  extensions required to carry authenticated consistency receipts are in scope
  and MUST preserve the legacy compatibility window.
- Automatic leader election, quorum consensus, global serializability,
  cross-identity requester takeover or a highly available replacement for
  ServiceController.
- Autonomous percentage-based canary rollout, multi-cluster federation, a
  general-purpose orchestration scheduler or a swappable distributed state-store
  SPI. Spec 111 includes immutable replace/rollback and reconciliation only.
- Replacing the native C++ execution implementation.
- Creating separate placement, resource-allocation, load-balancing, scaling,
  parallelism, communication, memory, speculative-execution or backend policies
  that duplicate the ten accepted policy authorities.
- Standardizing an independently swappable `CostModel`/`PerformanceModel` in
  this revision. An optimization package may share one internally; a public
  estimator SPI requires separate evidence that multiple suites need to
  exchange the same estimator.
- Re-running or relabeling historical Spec 107/109/110 evidence.
- Treating an iTiger Slurm allocation as an indefinite public-IP Docker service,
  or making Spec 111 itself submit a live Slurm job.
- Building or publishing OCI, materializing/running SIF, invoking Docker,
  Podman, Buildah or Apptainer, or contacting iTiger from Spec 111 validation.
- Bundling Qwen weights into a package or container.
- Using the login node for persistent services or unbounded builds/jobs.
