# Research: NDNSF-DI Core/APP Separation

## Decision 1: Treat the current documentation boundary as authoritative

**Decision**: Preserve the accepted rule that NDNSF Core owns service-neutral
network/security facts and DI owns model execution semantics, while adding an
internal DI split between execution mechanism and application policy.

**Rationale**: `docs/ndnsf-core-app-boundary.md` already assigns model split,
fragment/cache semantics and DI scheduling to DistributedInference. The user's
new concern is one level lower: those DI application policies are mixed with the
DI execution implementation and publishing surface.

**Alternatives considered**:

- Move DI policy into NDNSF Core: rejected because it violates the accepted
  application boundary.
- Treat the current package as intentionally monolithic: rejected because its
  root interface and `runtime_v1.py` couple unrelated change reasons.

## Decision 2: Extract behavior before changing policy

**Decision**: Make the first implementation increment a characterization-backed
module extraction. Preserve current score results, commands, schemas and native
behavior through thin compatibility adapters.

**Rationale**: The current runtime and experiments have candidate-bound evidence.
Combining file movement with new optimization would make regressions impossible
to attribute and could invalidate Spec 110 continuation.

**Alternatives considered**:

- Rewrite Runtime v1 around new abstractions: rejected as unnecessarily risky.
- Immediately split repositories/distributions: rejected because dependency
  direction is not yet mechanically clean.

## Decision 3: Unify Provider assignment while separating policy and mechanism

**Decision**: One `ProviderAssignmentPolicy` owns ordinary one-role and
multi-role objectives/ranking. Core owns eligibility, decision validation,
lease application, assignment binding and bounded recovery.

**Rationale**: Moving all Provider selection into APP would duplicate or bypass
lease, stale telemetry, feasibility and attempt-epoch invariants. Keeping the
hard-coded score in Core prevents application-specific optimization. A policy
seam separates both concerns without adding a network planner service.

**Alternatives considered**:

- APP returns an unchecked assignment: rejected as unsafe.
- Core exposes only one configurable weight dictionary: rejected because it
  still fixes the objective shape and cannot express fixed/manual policies.

## Decision 4: Reuse the native execution seam

**Decision**: Retain `NativeExecutionPlan`, `NativeProviderAssignment`,
`NativeProviderSession`, `NativeProviderRuntime`, `DependencyIo` and
`NativeModelRunnerFactory` as the behavioral reference.

**Rationale**: These interfaces already separate plan, assignment, dependency
transport and runner implementation. They provide more module depth than the
current Python publishing surface.

**Alternatives considered**:

- Move or rename the native C++ hierarchy in the same feature: rejected because
  it adds ABI/build risk without resolving the Python policy mixture.

## Decision 5: Replace ambient placement state with immutable request context

**Decision**: Pass an explicit `AssignmentContext` for each request and forbid
writes to `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE`.

**Rationale**: The current helper temporarily mutates a process-global
environment variable. A lock would serialize rather than correctly model
independent concurrent assignments and would still lose policy/evidence lineage.

**Alternatives considered**:

- Global mutex around environment mutation: rejected as ambient and
  non-composable.
- One process per request: rejected as an operational workaround, not a safe
  library contract.

## Decision 6: Isolate profiles before physical distribution split

**Decision**: First prove Core-only imports and dependency inventories, then split
physical distributions after compatibility callers are migrated.

**Rationale**: A package rename does not guarantee architectural separation; an
import graph and isolated profile do. This order also provides a rollback
aggregate while the source tree is changing.

**Alternatives considered**:

- Maintain one permanent all-inclusive distribution: rejected because it cannot
  provide the requested pure Core installation.
- Split distributions first: rejected because current root exports and imports
  would create cyclic or duplicated compatibility implementations.

## Decision 7: Candidate evidence is immutable across the refactor

**Decision**: Preserve historical evidence bytes/digests and assign any
post-separation local, MiniNDN, container or iTiger execution a new candidate
identity.

**Rationale**: Source movement changes the tested candidate even when behavior is
intended to be equivalent. Evidence integrity requires explicit new lineage and
prevents a structural refactor from inheriting remote performance claims.

**Alternatives considered**:

- Treat behavior-preserving source as the same candidate: rejected because the
  source/release digest changes.
- Re-run all historical experiments inside Spec 111: rejected because it
  broadens scope and conflicts with Spec 110 ownership.

## Decision 8: Compatibility is bounded and observable

**Decision**: Every legacy export/command/config mapping has one canonical owner,
usage observation, exit criteria and rollback release. It is removed only after
two consecutive zero-caller inventories and external migration gates.

**Rationale**: Unbounded re-exports recreate the same mixed interface under new
files. Immediate deletion would violate migration safety.

**Alternatives considered**:

- Permanent umbrella exports: rejected as a failure to complete separation.
- Date-only expiry: rejected because external callers and validation may not be
  ready by an arbitrary date.

## Decision 9: Make external algorithms first-class installed extensions with
deep, non-overlapping seams

**Decision**: Publish one optimization SDK with ten typed Python policies for
deployment, conditional model-variant choice, partitioning, unified ordinary/multi-role Provider assignment,
scheduling/dispatch, bounded execution tuning, cache choice, admission,
recovery and execution-target selection. APP composes them in an
instance-scoped `OptimizationSuite`. Runner creation is a separate
`RunnerAdapter` factory SPI registered in an instance-scoped
`ExecutionAdapterRegistry`; Core/provider mechanisms validate every proposal
and adapter selection.

Core owns the minimal mechanism-facing decision-port request/result schemas and
validators. The SDK re-exports/adapts them for external authors. This preserves
dependency inversion without duplicating contracts or making Core import SDK.

**Rationale**: Current code is only partially extensible. `PlannerBackendRegistry`
can call an external split handler and `APPProvider` accepts a Python inference
handler, but these seams are manually wired and fragmented. Runtime v1 hard-codes
placement/scheduling/cache objectives, collaboration role assignment remains in
the native selector, and APPClient cannot accept a complete optimizer. A stable
SDK is required for the independent algorithm team to work without repository
edits or internal imports.

All existing policy algorithms become a versioned `DefaultOptimizationSuite`
using the same public policies. Existing runner implementations become named
registrations in `DefaultExecutionAdapterRegistry`. A closed decision inventory
covers Runtime v1, deployment, built-in ACK assignment, Qwen/transfer dispatch,
exact/semantic/KV cache and execution-target choices so “external optimization”
cannot mean only placement.

Ordinary ACK Provider selection is the one-role/multiplicity form of multi-role
assignment, so a separate `ProviderSelectionPolicy` would duplicate candidate,
ranking and validation contracts. Scheduling owns both ordering and the bounded
dispatch capacity granted to that order; per-invocation segment/microbatch/
compression parameters remain in `ExecutionTuningPolicy`. Cache policy emits
affinity but does not choose the compute Provider. Recovery selects a transition
and delegates replacement decisions. Planner registry lookup is explicit
configuration, while `ExecutionTargetPolicy` proposes compatible
runtime/engine/device candidates and unified assignment selects their bindings,
adapter identity. These boundaries preserve typed validation without redundant
shallow peer interfaces.

`DeploymentPolicy` remains necessary because lifecycle/profile selection occurs
before split generation and request-time Provider assignment; it cannot author
either result. `AdmissionPolicy` remains separate because it decides whether an
incoming invocation may enter the ready-work set, while `SchedulingPolicy`
orders and grants bounded capacity only to already admitted, dependency-ready
work. Thus neither an empty dispatch nor a deployment profile can become a
second admission or assignment path. Cache-object admission is separately
scoped to cache state and cannot admit or reject an inference invocation.

For joint partition/placement algorithms, `PartitionPlanner` returns an
immutable plan candidate set and `ProviderAssignmentPolicy` selects the plan
identity together with assignments. One external implementation may back both
protocols with a common stateless solver.

**Alternatives considered**:

- One universal optimizer callback: rejected because it would expose unrelated
  authority and create a shallow, stateful interface.
- The former eleven peer-port proposal: rejected because it duplicated Provider assignment, split
  scheduling capacity from ordering, conflate planner lookup with runtime target
  choice, and classify a Runner factory as an optimization policy.
- Configuration-only weights: rejected because it cannot express new
  algorithms, split structures or runner backends.
- A network planner service: rejected because it adds protocol/authority and is
  unnecessary for in-process application policy.

## Decision 10: Explicit registration first; discovery is opt-in and allowlisted

**Decision**: Direct instance registration is mandatory. Standalone wheels may
also expose `ndnsf_di.optimizers` entry points, but APP loads them only after an
explicit request and distribution name/version/digest verification.

Allowlisting verifies installed identity; it is not a code sandbox.
Deployment hooks execute under APPDeployment, client-side hooks under APPClient,
and provider-side hooks under APPProvider. Worker processes provide
crash/late-work/resource containment only.

**Rationale**: Python entry points make third-party installation convenient but
execute arbitrary installed application code. Automatic import during Core load
would violate isolation, create hidden global state and weaken reproducibility.

**Alternatives considered**:

- Import-time auto-discovery: rejected for security, startup determinism and
  state-isolation reasons.
- Environment-variable plugin selection: rejected because Spec 111 is removing
  ambient per-process policy state.

## Decision 11: Require separate artifacts and out-of-tree acceptance samples

**Decision**: Feature completion requires separately installable Core, SDK,
APP/planner and selected model-adapter artifacts, a standalone external Python
optimizer wheel, and an out-of-tree native runner build against installed public
headers.

**Rationale**: An import test inside the repository cannot prove that another
group can consume the framework. Clean installation is the direct acceptance
test and resolves the audit's deferred physical-distribution concern.

**Alternatives considered**:

- Keep profiles only inside one wheel: rejected because it does not prove
  independent external consumption.
- Runtime `dlopen` of arbitrary native plugins: rejected because stable C++ ABI,
  compiler/runtime compatibility and code-loading security are outside this
  refactor.

## Decision 12: Add an engine composition root, shared objective/snapshot and conditional model selection

**Decision**: Define a process-local APP-owned `DistributedInferenceEngine`
over a machine-readable decision DAG, shared `OptimizationObjective` and
lineage-bound `EngineSnapshot`. Expand the public surface from nine to ten
policies by adding `ModelVariantPolicy`, invoked only when APP/operator input
authorizes multiple semantically acceptable model variants under a quality
contract. The policy proposes bounded candidates; final model selection is joint
with variant-bound partition and Provider assignment. Refine deployment,
scheduling, tuning and cache contracts rather than
adding separate scaling, batching, communication or memory policies.

**Rationale**:

- CodeGraph finds no current `DistributedInferenceEngine` symbol; orchestration
  is fragmented across `DistributedInferenceClient`, `APPClient`, deployment,
  Runtime v1, provider admission, `NativeProviderRuntime`,
  `ProviderRoleWorker` and generation schedulers.
- Public policy interfaces without an invocation graph leave ordering,
  invalidation, recovery re-entry and evidence ownership ambiguous.
- Qwen size/revision/precision/quantization selection has no correct owner:
  partitioning decides how a selected model is split; execution target decides
  where it runs; deployment decides lifecycle. A conditional model-variant port
  closes the quality/latency/cost choice without weakening exact constraints.
- The current scheduler evidence is FIFO/deadline/worker-bounded and the Qwen
  scheduler owns whole generations; complete distributed-inference optimization
  therefore needs typed dynamic-batch, prefill/decode, fairness, preemption and
  proactive-hedge alternatives in `SchedulingPolicy`.
- Current deployment/session behavior mainly publishes metadata or ranks
  ACTIVE/IDLE/DISK/EVICTED records; scaling, prewarm and unload must be explicit
  lifecycle actions before deployment optimization can claim completeness.
- One closed cache policy with phase/action discriminators preserves shared
  state authority better than separate exact/semantic/KV policy families.

**Alternatives considered**:

- Keep nine ports and treat model choice as partition or target selection.
  Rejected because it mixes semantic quality choice with layout or runtime
  compatibility and permits silent model replacement.
- Add separate placement, resource allocation, load balancing, scaling,
  parallelism, communication, memory, speculative execution and backend
  policies. Rejected as over-factored: their authoritative choices already map
  to assignment, scheduling/tuning, deployment, partition, cache/Core and
  execution target.
- Add an independent public `CostModel`/`PerformanceModel` SPI now. Deferred:
  one external suite can share its estimator internally; a cross-suite estimator
  contract needs separate interoperability evidence.
- Implement a central network planner/coordinator. Rejected because it changes
  distributed authority and protocol scope; the engine is only process-local
  composition over existing NDNSF execution.

## Decision 13: Add joint-decision, scoped-control, atomicity and feedback completeness without new policies

**Decision**: Keep ten policies, but add typed scheduling/admission scopes,
metric/estimate semantics, adapter capabilities, atomic execution intent,
streaming progress/checkpoint facts and one optional independent outcome-
observer SPI. `ModelVariantPolicy` returns candidates, not one prematurely
selected model. Runner creation and outcome observation are the only two
non-policy SPIs in this revision.

**Rationale**:

- A single model choice before partition/provider feasibility prevents genuine
  joint model/parallelism/resource optimization. Variant-bound plan candidates
  plus final assignment selection preserve typed authority while allowing a
  package to use one internal optimizer.
- A target chosen only after Provider assignment hides device/runtime cost and
  compatibility from joint placement. Target policy therefore proposes bounded
  plan-role-Provider candidates and assignment selects their IDs with the tuple.
- Cross-role DAG/transfer dispatch and provider-local continuous batching act
  on different state and have different owners. Scope discriminators avoid two
  policies while preventing APPClient from mutating local queues or APPProvider
  from inventing cross-role assignments.
- Modern engines do not share one universal batching rule. TensorRT-LLM
  documents in-flight batching as KV-capacity dependent, and both TensorRT-LLM
  and vLLM support disaggregated prefill/decode with explicit KV-transfer
  connectors/capabilities. Therefore compatibility comes from the selected
  adapter, not a fixed token-phase equality rule.
- DistServe jointly optimizes phase-specific resource allocation/parallelism
  under separate TTFT and TPOT requirements, confirming that objective metrics
  need explicit direction, units and aggregation rather than an unnormalized
  weight dictionary.
- Ray Serve exposes separate queue/admission and autoscaling controls plus
  look-back/aggregation/up/down delays, supporting typed engine/provider
  admission and deployment cooldown/hysteresis instead of unconstrained
  per-request scale actions.
- Independent policy proposals may each be valid but mutually stale by the time
  side effects begin. One Core/provider prepare/revalidate/commit-or-abort
  mechanism prevents torn decisions without creating a transaction policy.
- External learning/evaluation needs measured outcomes and state lineage; a
  one-way off-path observer supplies that feedback without giving it current-
  request authority or adding a Core persistence engine.

**Primary calibration sources**:

- vLLM, “Disaggregated Prefilling (experimental)”: separate TTFT/ITL tuning,
  connector-owned KV transfer, and the explicit warning that disaggregation is
  not a universal throughput improvement: <https://docs.vllm.ai/en/v0.18.1/features/disagg_prefill/>.
- NVIDIA TensorRT-LLM, “Disaggregated Serving” and “Memory Usage”: phase-specific
  resources, modular KV exchange/overlap, heterogeneous layouts, in-flight
  scheduling under KV capacity: <https://nvidia.github.io/TensorRT-LLM/1.3.0rc20/features/disagg-serving.html>
  and <https://nvidia.github.io/TensorRT-LLM/reference/memory.html>.
- Zhong et al., DistServe, OSDI 2024: joint phase-specific allocation,
  parallelism and placement under TTFT/TPOT requirements:
  <https://www.usenix.org/system/files/osdi24-zhong-yinmin.pdf>.
- Ray Serve, “Advanced Autoscaling”: queue/ongoing-request bounds, look-back
  aggregation and asymmetric scaling delays:
  <https://docs.ray.io/en/latest/serve/advanced-guides/advanced-autoscaling.html>.

**Alternatives considered**:

- Add `SolutionSelectionPolicy`, separate global/local schedulers, separate
  admission policies, a transaction policy, state-store SPI and estimator SPI.
  Rejected as over-factoring: existing policies can use typed scopes and one
  internal joint solver; distributed certification/activation is mechanism; state/estimation are
  suite-owned until independent interoperability is demonstrated.
- Make observer synchronous or let it alter the just-completed decision.
  Rejected because feedback failure would become inference failure and would
  create a second authority path.

## Decision 14: Reuse generic leases and require a complete authenticated commit certificate

**Decision**: Keep `GenericExecutionLease`, `ProviderExecutionLeaseTable`, the
authenticated Targeted lease service, V2 request/Selection path and
`ExecutionAttemptAuthority` as the canonical mechanism. The initiating
`APPClient` identity coordinates each request attempt; Providers issue
authenticated prepare/commit receipts under their boot epochs; execution becomes
visible only after a complete immutable `ExecutionCommitCertificate` binds the
entire Provider receipt set. A DI-Core companion verifier composes the generic
lease table rather than adding DI fields to its public ABI. Deployment actions use a separate single-writer
lifecycle epoch and compare-and-set action certificate. Periodic Provider cleanup
reclaims orphans independently of subsequent traffic.

**Rationale**:

- Current C++ leases already encode Provider identity/boot epoch, request,
  service, plan digest, binding proof, conflicts, idempotency, expiry and the
  `PREPARED/COMMITTED/EXECUTING/ABORTED/RELEASED/EXPIRED` states.
- Current Python `DistributedLeaseTransaction` already performs prepare-all,
  commit-all and best-effort rollback. Replacing it would create two lease
  authorities. What is missing is durable evidence that every selected Provider
  committed before any Provider activates work.
- An authenticated complete receipt set gives atomic execution visibility while
  bounded lease expiry handles unreachable participants. It does not falsely
  promise simultaneous global transition or availability under partition.
- Attempt epoch, Provider boot epoch and deployment lifecycle epoch fence stale
  coordinators, restarted Providers and conflicting lifecycle writers without
  introducing leader election or a cluster database.

**Alternatives considered**:

- Keep only best-effort abort/release: rejected because a lost commit response
  leaves the coordinator unable to distinguish a committed reservation from a
  failed operation, and a partial set must never become executable.
- Add a second DI-specific lease protocol or top-level NDN coordinator name:
  rejected because it duplicates existing Targeted lease/security semantics and
  expands protocol authority before the Engine API is stable.
- Claim strict distributed two-phase-commit atomicity: rejected because network
  partitions and coordinator failure can leave bounded in-doubt reservations;
  the enforceable property is all-receipt execution visibility plus fencing and
  expiry.
- Add Raft/quorum leader election or global serializable storage: rejected as
  disproportionate to requester-coordinated inference and outside Spec 111.
- Permit another requester identity to take over automatically: rejected because
  identity authority is security-sensitive; cross-identity delegation must be an
  explicit future contract.

## Decision 15: Make deployment revision and durable request handles the public workflow

**Decision**: Extend the existing deployment configuration into a canonical
`DeploymentDefinition` and immutable `DeploymentRevision`; make APPDeployment
own validate/resolve/plan/apply/status/wait/rollback/drain/delete; make APPClient
return a durable `InferenceRequestHandle`; and persist lifecycle/request recovery
evidence in one fixed APP-owned filesystem `RuntimeJournal`. Rename the
metadata-only client session to `PreparedPlanSession`, retaining
`deploy_plan()`/`DeploymentSession` as bounded compatibility aliases.

**Rationale**:

- Current `APPDeployment` only exposes configuration accessors and performs no
  deployment operation. A separated package would otherwise still lack the
  operator action that turns a definition into ready Providers.
- Current `DistributedInferenceClient.deploy_plan()` publishes/caches static
  plan/artifact references and explicitly does not execute or activate a
  deployment. Its name is unsafe as the only high-level deployment story.
- Process-local `Future` cannot reopen a request after requester crash, although
  the consistency contract requires durable certificate/result rendezvous.
- `RuntimeJournal` is fixed mechanism state, not optimizer learning state or a
  public state-store SPI. A mounted append-only journal supplies restart and
  rollback evidence without introducing consensus or a cluster database.
- A reopenable request also requires recoverable input: the existing protected
  RequestMessage wire envelope is persisted in an owner-only spool or durable
  NDN repository, while only its name/digest/security/expiry enters the journal.
- External container/Slurm/systemd/operator layers launch generic Provider
  agents; APPDeployment owns revision/model lifecycle inside those agents, not
  infrastructure process provisioning.
- Immutable revisions bind external artifacts, adapters, policy and security
  inputs so Provider readiness, request assignment, upgrade and rollback share
  one identity.

**Alternatives considered**:

- Treat configuration generation as deployment: rejected because policy/trust
  files do not prove Provider staging, warming, readiness or activation.
- Keep `deploy_plan()` as the canonical deployment API: rejected because its
  current and desired behavior is reusable metadata publication, not lifecycle
  application.
- Let operations CLI directly orchestrate NFD, controller and Provider state:
  rejected because it creates a second lifecycle implementation and confuses
  external process supervision with APP authority.
- Make every request synchronous/in-memory: rejected because requester restart,
  cancellation and result rendezvous would remain undefined.
- Add a pluggable database or consensus service: rejected until multi-host APP
  control-plane availability becomes a proven requirement.
- Journal raw prompts or retry from process memory: rejected because the former
  violates least-input/at-rest boundaries and the latter makes a durable handle
  unrecoverable after crash.
- Make APPDeployment launch Provider OS/container/Slurm processes: rejected for
  this feature because it duplicates deployment-adapter/scheduler authority and
  creates a bootstrap cycle before revision selection.
- Bundle model weights in revisions/images: rejected because weights are large,
  independently retained artifacts and must remain external digest-bound inputs.

## Decision 16: Treat iTiger as a Slurm/Apptainer batch target, not a Docker service

**Decision**: Keep the Docker image as the sealed OCI build/distribution source,
materialize an exact SIF, and let the existing operations-owned
`slurm-apptainer` adapter launch infrastructure. Bind a Spec 111 deployment
revision to that launch through an immutable `RuntimeAllocationHandoff` and keep
the scheduler, deployment and request handles distinct.

Spec 111 validates only the schema, render output and ownership boundary with
fixtures. It deliberately defers the first post-separation OCI build, SIF
materialization, Apptainer execution and iTiger job to Spec 110, after local and
MiniNDN implementation gates pass.

**Rationale**:

- iTiger supplies Slurm and Apptainer on compute nodes; a Docker daemon and
  long-lived public service are neither required nor assumed.
- APPDeployment is intentionally not an OS/container/job scheduler. The runtime
  adapter must launch generic Provider agents before APP applies a revision.
- A pre-Spec-111 image cannot contain or prove the new APP SDK, RuntimeJournal,
  readiness and durable request-handle implementation.
- The existing canonical runner already enforces exact SIF and explicit model/
  artifact/identity/scratch binds, but it lacks the post-Spec-111 persistent
  state bind and its generic allocation adapter still renders one workload.
- The existing allocation-topology implementation is useful mechanism but its
  v1 validator fixes three Providers and its generated project commands can
  resolve from the host unless a container wrapper is part of each command.

**Alternatives considered**:

- Run Docker directly on iTiger: rejected because it is not the supported
  compute runtime and would require daemon/NVIDIA-container configuration outside
  the user allocation contract.
- Put Slurm submission inside APPDeployment: rejected because it conflates
  infrastructure and model/revision lifecycle and makes local/MiniNDN use depend
  on an HPC scheduler.
- Treat `sbatch` acceptance or job RUNNING as deployment success: rejected
  because no Provider revision, permission, artifact, backend or request evidence
  exists at those states.
- Reuse the current Spec 110 OCI/SIF after Spec 111: rejected as candidate
  relabeling; it may prove only unchanged substrate capabilities.
- Hard-code the existing three Qwen stages in the generic adapter: rejected
  because partition/cardinality belongs to the immutable revision and external
  optimizer result.
- Expose compute-node NFD by public IP for indefinite remote calls: rejected as
  incompatible with bounded scheduler ownership and the no-persistent-service
  boundary; authorized interactive allocations remain possible.

## Code Reality Summary

- `ndnsf_distributed_inference/__init__.py` exports APP, runtime, placement,
  cache, LLM, ONNX, planner and splitter surfaces together.
- `runtime_v1.py` combines contracts, scoring, cache/state, simulations,
  production adapters and CLI.
- Current policy-bearing choices also exist outside Runtime v1: deployment
  status ordering, NDNSF ACK built-ins, ONNX split ranking, Qwen and dependency
  queue scheduling, runtime/ONNX target fallback, and semantic-cache ranking,
  admission, eviction and Provider affinity. A placement-only SDK would not
  satisfy the user's complete external-Python requirement.
- `deployment.py` transports request placement through the global
  `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE` environment variable.
- `APPDeployment.from_config()` only wraps `load_or_generate_deployment()` and
  exposes trust/policy/role/model accessors; it has no apply/status/wait/
  rollback/drain/delete or restart-reconciliation operation.
- `DistributedInferenceClient.deploy_plan()` explicitly publishes static plan
  metadata and returns a process-local `DeploymentSession`; it does not load or
  activate Providers. Async invocation returns a process-local `Future`, not a
  durable request handle.
- Runtime v1 has status/metrics/doctor command adapters, but no canonical APP
  lifecycle API behind validate/apply/wait/rollback/drain/delete and no fixed
  durable APP journal tying those commands to consistency certificates.
- `planner_registry.py` already has a useful empty registry and explicit backend
  seam.
- Native C++ session/runtime/runner/dependency interfaces already form a strong
  execution seam.
- Existing runtime-aware planner tests cover score and assignment behavior;
  new tests are needed for dependency direction, policy/Core authority,
  concurrent assignment isolation and compatibility exit.
- Current code has a generic multi-Provider prepare-all/commit-all lease
  transaction with Provider boot-epoch and TTL enforcement, but it has no
  complete authenticated commit-certificate activation gate, requester-crash
  recovery contract, deployment single-writer lifecycle fencing or guaranteed
  idle-time orphan sweep. It also has no stable decision-to-outcome observer,
  policy-state epoch/digest or Core-owned streaming checkpoint/output-commit
  recovery contract.
- `ServiceUser.request_service_select()` exposes a custom selector for ordinary
  service calls, but `request_collaboration()` exposes only an observational ACK
  callback and the native `RoleAssignmentSelectionPolicy` makes multi-role
  assignments internally.
- `RegistryNativeModelRunnerFactory` and `NativeProviderRuntime.registerRunner`
  are valid C++ seams, but there is no complete standalone external SDK/build
  acceptance path today.
