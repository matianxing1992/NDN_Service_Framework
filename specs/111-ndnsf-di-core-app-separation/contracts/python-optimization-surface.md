# Contract: Complete Python Optimization Surface

## Normative completeness rule

Every NDNSF-DI runtime decision that does any of the following is a
**policy-bearing decision point** and MUST execute through a public Python
policy:

- chooses one or more alternatives from an otherwise valid candidate set;
- scores, ranks, dispatches or prioritizes candidates or ready work;
- generates or selects a deployment, split, provider assignment or recovery
  direction;
- ranks/prunes model/revision/tokenizer/precision/quantization/draft/adapter
  candidates from an APP/operator-authorized alternative set;
- chooses cache reuse, admission, state placement, prefetch or eviction;
- proposes compatible execution runtime, engine, device or fallback candidates;
- forms a compatible dynamic/continuous dispatch batch, token-phase order,
  preemption/resume or proactive hedge;
- chooses bounded microbatch, segment, compression, transfer, prefetch, overlap
  or speculative-decoding tuning parameters.

Parsing, normalization, cryptographic/security checks, exact identity matching,
schema validation, eligibility filtering, dependency readiness, lease/deadline
enforcement, artifact verification and stale-result rejection are mechanisms,
not optimization decisions, and MUST NOT be externally overrideable.

An operator's exact explicit choice is a constraint. A Python policy may fill an
unspecified choice or optimize within allowed alternatives, but cannot silently
replace an exact operator constraint.

## Public policy surface

All policies are exported by `ndnsf_distributed_inference.sdk`, accept immutable
Core-owned request types, return typed proposals, and expose one public method.
The policy surface contains ten policies. Two independent non-policy SPIs sit
beside it: `RunnerAdapter` creates execution runners and optional
`OptimizationObserver` receives outcome feedback. Neither is an eleventh policy.

| Policy | Method | Covers | Non-overridable validation |
| --- | --- | --- | --- |
| `DeploymentPolicy` | `plan` | idempotent control-plane/session-epoch use-existing, activation, reservation, prewarm, scale-out/in, drain and unload proposal with cooldown/residency state | identity, lifecycle epoch, active lease/session, cooldown/drain, exact constraints, capacity and reservation validity |
| `ModelVariantPolicy` | `propose` | bounded rank/prune of authorized model family/size/identity, revision, tokenizer, precision, quantization, draft and adapter alternatives | exact model/semantic constraints, alternative membership, quality contract and artifact/tokenizer identity |
| `PartitionPlanner` | `plan` | graph split candidates, layer/shard allocation, local/distributed layout and plan candidates | plan schema, artifacts, dependencies and resource feasibility |
| `ProviderAssignmentPolicy` | `assign` | joint authorized variant/plan/Provider/role-target selection with one-role or multi-role assignment, multiplicity, cost/pack/spread and cache-affinity consumption | variant/plan/target binding, successful ACK membership, token/permission, eligible snapshot, leases, freshness, resources and multiplicity |
| `SchedulingPolicy` | `dispatch` | `REQUEST_DAG` role/transfer ordering and assignment-authorized hedging, or `PROVIDER_LOCAL` adapter-compatible dynamic/in-flight batching, phase arbitration, fairness, preemption/resume and bounded grants | declared scope, readiness, adapter batch capability, queue/assignment membership, deadline and mechanism hard limits |
| `ExecutionTuningPolicy` | `tune` | declared typed/ranged segment, microbatch, compression, transfer-chunk, prefetch-depth, overlap and speculative-decoding-window parameters | declaration, type/range/scope, memory, deadline and provider safety limits |
| `CachePolicy` | `decide` | phase-tagged exact/semantic/prefix/KV lookup/reuse, place, prefetch, admit, retain, evict and migrate/replicate; emits affinity but does not choose compute Provider | action/phase, key/security/epoch validity, storage capacity and atomic state update |
| `AdmissionPolicy` | `decide` | `ENGINE_REQUEST` tenant/quota/SLO/backpressure or `PROVIDER_LOCAL` readiness/capacity accept/defer/reject | prior-scope rejection and mandatory floors; policy can only be stricter |
| `RecoveryPolicy` | `decide` | choose one Core-allowed recovery directive and checkpoint boundary: retry/resume/restart/reassign/repartition/redeploy/defer/fail | original deadline, attempt/output-commit budget, exclusions and stale/duplicate-result rejection |
| `ExecutionTargetPolicy` | `propose` | bounded compatible runtime/engine/device/adapter candidates for each plan-role-Provider alternative plus explicit fallback | installed adapter registration, artifact/device/batch/checkpoint compatibility, candidate bounds and actual availability |

`ProviderAssignmentPolicy` deliberately unifies ordinary Provider selection and
multi-role placement. An ordinary service request is represented as one
synthetic role with requested multiplicity; collaboration/inference requests use
one or more explicit roles. Both paths consume eligible ACK/capability facts and
produce a validated Provider assignment.

`ModelVariantPolicy` is applicable only when the immutable
`ModelAlternativeSet` contains at least two semantically acceptable variants
under an explicit quality metric/floor. One exact model constraint records
`NOT_APPLICABLE_EXACT_CONSTRAINT` and becomes a singleton; no policy is called
to replace it. Otherwise the policy returns a bounded candidate set,
`PartitionPlanner` emits variant-bound plan candidates, and
`ExecutionTargetPolicy` proposes compatible targets for each plan-role-Provider
alternative. `ProviderAssignmentPolicy` then selects the final authorized
variant/plan/Provider/target tuple.

`DeploymentPolicy` chooses among deployment-lifecycle alternatives such as an
existing deployment record/profile, activation, reservation, prewarm,
scale-out/in or unload/evict action at a deployment/session epoch. It does
not generate a model split, choose request-time compute Providers or select a
model/runtime device, and is not invoked per token or unconditionally per
request. Those decisions remain with `ModelVariantPolicy`, `PartitionPlanner`,
`ProviderAssignmentPolicy` and `ExecutionTargetPolicy`, respectively.
Lifecycle actions carry idempotency, readiness, cooldown/minimum-residency and
drain preconditions so scaling does not oscillate or terminate active work.

`SchedulingPolicy` owns dispatch ordering and bounded capacity under two typed
scopes: APPClient owns cross-role/transfer `REQUEST_DAG` dispatch and hedges only
to assignment-authorized replicas; APPProvider owns `PROVIDER_LOCAL` compatible
batching, phase arbitration, fairness and preemption/resume. Batch compatibility
comes from the selected Runner adapter, so mixed prefill/decode is allowed only
when advertised. Post-failure transitions remain recovery.
`ExecutionTuningPolicy` does not independently
choose queue windows or worker concurrency; it tunes the already
admitted/dispatched invocation using only a declared `TuningParameterSpec`.

`AdmissionPolicy` uses `ENGINE_REQUEST` scope for quota/SLO/backpressure before
expensive planning and `PROVIDER_LOCAL` scope for local readiness/capacity after
assignment. Either rejection remains binding. `SchedulingPolicy` sees only
admitted, dependency-ready work and cannot reverse admission.

`CachePolicy` may place cache state, but it cannot make the final compute
Provider assignment. Cache hit/value/affinity facts are inputs to
`ProviderAssignmentPolicy`. Its “admission” decision means whether a cache
object enters a cache; it does not admit or reject an inference invocation,
which remains exclusively `AdmissionPolicy` ownership.
Pre-assignment LOOKUP/REUSE/PLACE/PREFETCH actions and post-execution
ADMIT/RETAIN/EVICT/MIGRATE_OR_REPLICATE actions are separate decision epochs
with one closed action enum and atomic state validation.

`RecoveryPolicy` selects a transition only. `REASSIGN`, `REPARTITION` and
`REDEPLOY` delegate the new decision to `ProviderAssignmentPolicy`,
`PartitionPlanner` and `DeploymentPolicy`, respectively; recovery cannot embed
competing assignment, partition or deployment algorithms.

Planner implementation lookup is explicit registry/configuration resolution,
not an optimization decision. `ExecutionTargetPolicy` therefore proposes only
runtime execution targets for later joint assignment. Exact requested planner identities are resolved by
the planner registry before `PartitionPlanner.plan` is called.

## Runner adapter SPI

`RunnerAdapter.create` remains a public extension seam, but it is an adapter or
factory SPI rather than an optimization policy and is not a member of
`OptimizationSuite`:

| SPI | Method | Covers | Non-overridable validation |
| --- | --- | --- | --- |
| `RunnerAdapter` | `create` | Python inference handler or registered native runner factory creation plus immutable `BatchCapability`/checkpoint/device capability advertisement | adapter identity, artifact/device/capability validation, provider permission, execution evidence and response authority |

Adapters are registered in an instance-scoped `ExecutionAdapterRegistry` by
stable adapter identity. `ExecutionTargetPolicy` proposes compatible adapter/
runtime/device candidates; `ProviderAssignmentPolicy` selects target identities
with Provider bindings, and the registry invokes `create` only after intent
commit. Merely registering an adapter never
selects or executes it.

## Outcome observer SPI

`OptimizationObserver.observe` is an optional independent SPI, not an eleventh
policy and not a Runner adapter. It receives a bounded immutable
`OptimizationOutcome` after commit/completion/failure on a separately budgeted
APP executor. It is idempotent by outcome identity, cannot affect the current
request, and records observer failure separately. Stateful extensions own
persistence/concurrency and record state epoch/digest on later decisions; Core
does not expose a plugin database.

## Current decision-point inventory and required defaults

Every row becomes a machine-readable record in
`specs/111-ndnsf-di-core-app-separation/contracts/decision-point-inventory.json`.
The source column names current logic to migrate, not a permanent implementation
location.

| Decision ID | Current source decisions | Public seam | Versioned default family |
| --- | --- | --- | --- |
| `DEPLOYMENT_SELECT` | `deployment.discover_deployments` status ordering and deployment activation/reservation choice | `DeploymentPolicy` | `ndnsf.default.deployment.status-first/v1` |
| `MODEL_VARIANT_PROPOSE` | no single current owner; exact model manifest/config and Qwen pilot choices are currently caller-owned | `ModelVariantPolicy`; exact constraints create singleton/not-applicable evidence | `ndnsf.default.model-variant.exact-or-compatible-set/v1` |
| `PARTITION_PLAN` | split planners, ONNX candidate ranking, `proportional_layer_allocation` and local LLM plan construction | `PartitionPlanner` | `ndnsf.default.partition.current/v1` |
| `SERVICE_PROVIDER_ASSIGN` | FirstResponding/Random/AllSelected and `request_service_select` | `ProviderAssignmentPolicy` with one synthetic role | named built-ins under `ndnsf.default.assignment.* /v1` |
| `ROLE_PROVIDER_ASSIGN` | Runtime v1 scoring, best-score/pack/spread/min-replicas, edge-aware and native role assignment | `ProviderAssignmentPolicy` with explicit roles | `ndnsf.default.assignment.runtime-cost/v1` |
| `READY_WORK_DISPATCH` | `RolePipelineScheduler.next_ready`, dependency transfer ordering/windows, `ProviderRoleWorker` FIFO and `BoundedGenerationScheduler` worker bounds | scoped `SchedulingPolicy` with request-DAG and provider-local adapter-compatible alternatives | `ndnsf.default.scheduling.current/v1` |
| `EXECUTION_TUNING` | `adaptive_segment_size`, microbatch, compression and bounded per-invocation parameters | `ExecutionTuningPolicy` | `ndnsf.default.tuning.bounded/v1` |
| `CACHE_DECIDE` | exact-forward LRU, semantic rank/admission/eviction, cache hints and KV state placement | `CachePolicy`; semantic compute-Provider choice moves to `ProviderAssignmentPolicy` | exact, semantic and KV defaults under `ndnsf.default.cache.* /v1` |
| `REQUEST_OR_PROVIDER_ADMISSION` | APP preflight/backpressure plus `ProviderAdmissionPolicy` and native readiness thresholds | scoped `AdmissionPolicy` | `ndnsf.default.admission.threshold/v1` |
| `RECOVERY_DIRECTIVE` | bounded retry/replan/fallback transition choice | `RecoveryPolicy`; fallback-plan generation and new assignment delegate to their owning policies | `ndnsf.default.recovery.bounded/v1` |
| `EXECUTION_TARGET_SELECT` | `default_runtime_backend` and ONNX execution-provider/device/CPU-fallback choice | `ExecutionTargetPolicy`; planner registry lookup is explicit configuration | `ndnsf.default.execution-target.compatibility-first/v1` |
| `RUNNER_CREATE` | APPProvider Python handler and native runner registries/factories | `RunnerAdapter` through `ExecutionAdapterRegistry` | registered existing runner adapters |

## Partition/assignment co-optimization

`ModelVariantPolicy.propose` returns a bounded authorized candidate set.
`PartitionPlanner.plan` returns variant-bound `PlanCandidateSet` entries rather
than one preselected layout. `ProviderAssignmentPolicy.assign` receives those
candidates with the eligible Provider snapshot and returns
`selected_model_variant_id`, `selected_plan_id` and role assignments. This
permits joint model/partition/placement scoring without one untyped universal
callback. Core validates the selected tuple independently and commits it only
through one `ValidatedExecutionIntent`.

## Default-suite rule

Existing policy behavior becomes `DefaultOptimizationSuite`; it is not retained
as a privileged internal branch. Existing runner implementations become named
members of `DefaultExecutionAdapterRegistry`. Each migrated default:

1. implements the same public policy or adapter contract available externally;
2. has a stable name, semantic version, configuration schema and digest;
3. is explicitly selected when APP does not provide that policy or adapter;
4. emits the same decision/creation evidence as an external implementation;
5. is covered by frozen before/after parity fixtures;
6. can be replaced per APP instance and request lineage without source edits,
   process-global mutation or framework rebuild.

Default policies are owned by Planner or corresponding model-adapter
distributions. The SDK owns contracts only. A selected policy or adapter failure
remains fail-closed unless APP explicitly names a fallback.

## Invocation granularity

Python policies operate on immutable batched snapshots at deployment, request,
attempt or provider scheduling epochs. They are not called once per NDN packet,
tensor segment or generated token. A validated proposal is reused only for its
declared input digest/epoch and is invalidated by changed eligibility, deadline,
capacity, cache epoch, model/deployment/runtime compatibility or exact operator
constraints. `DeploymentPolicy` is control-plane/session scoped;
`ModelVariantPolicy` is skipped under one exact model constraint; `CachePolicy`
may run at distinct pre-assignment and post-execution epochs. Policies receive
workload shape, not prompt/tensor payload, by default. Observer calls are
post-decision and separately budgeted, never per-token hot-path callbacks.

## Inventory enforcement

The decision inventory records source symbols/callers, public seam, invocation
owner, validator/reason codes, default identity, compatibility mapping and test
evidence. Static tests fail when a new policy-bearing decision is unclassified.
A mechanism-only classification must name the non-overridable invariant.

Generic NDNSF Core may retain built-in C++ ACK policies for non-DI callers.
NDNSF-DI paths adapt ordinary and multi-role assignment through
`ProviderAssignmentPolicy`; native code validates/applies the Python-produced
assignment and cannot make a competing DI assignment choice.

## External replacement acceptance

A standalone fixture wheel supplies deterministic non-default implementations
for all ten policies, independently registers a `RunnerAdapter` and optionally
an idempotent `OptimizationObserver`. Tests prove
that each policy changes its intended proposal or dispatch and that an adapter
is created only after `ExecutionTargetPolicy` proposal, joint assignment and
`ExecutionAdapterRegistry` resolution, while:

- Core/provider validators reject invalid proposals;
- omitted policies resolve to named defaults;
- no hidden current/default branch executes after replacement;
- two suites and adapter registries run concurrently without state bleed;
- observer failure cannot change current-request result or Core availability;
- no repository path injection, private import, source edit or rebuild is used.

Default parity is exact for deterministic decisions. For characterized
stochastic defaults, parity means the same admissible set/multiplicity and
predeclared distributional contract plus supplied-seed replay.
