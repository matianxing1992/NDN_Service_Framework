# Contract: External Optimization Extension SDK

## Goal

An independent team can ship optimization code as its own package and use
NDNSF-DI through stable interfaces. Extension code proposes application policy;
Core/provider mechanisms retain correctness, security and authority.

## Public package shape

Canonical Python contracts are exported from
`ndnsf_distributed_inference.sdk`. External packages MUST NOT import
`runtime_v1`, compatibility modules, private names, model implementations or
experiment helpers.

Core owns minimal immutable decision-port request/result types and mechanism
validators under `ndnsf_distributed_inference.core`. The SDK re-exports/adapts
those exact types; it does not define a second wire/schema or require Core to
import SDK code.

The SDK exposes:

- immutable request/result data classes;
- one-method `Protocol` interfaces;
- `OptimizationSuite` and an instance-scoped `OptimizationRegistry`;
- process-local `DistributedInferenceEngine` configuration over a shared
  `OptimizationObjective`, `EngineSnapshot` and `EngineDecisionGraph`;
- `RunnerAdapter` plus an instance-scoped `ExecutionAdapterRegistry`, and an
  optional `OptimizationObserver`, all outside the policy suite;
- an opt-in allowlisted entry-point loader;
- reusable contract/negative/replay tests;
- stable error and reason-code types.

The complete normative port list and current-source coverage are defined in
`python-optimization-surface.md`. Adding a selection/optimization decision
without updating that inventory is a contract violation.

The canonical policy entry-point group is `ndnsf_di.optimizers`; an entry point
returns an `OptimizationSuite` or factory. Runner adapters use
`ndnsf_di.runner_adapters` and return registrations for one
`ExecutionAdapterRegistry`. Merely installing either package does not load it.

## Shared descriptor and invocation envelope

Every extension supplies `OptimizationExtensionDescriptor`:

- `name`, `semantic_version`, `contract_version`;
- source/distribution digest and optional build identity;
- supported hook kinds and model/runtime capabilities;
- deterministic/stochastic declaration;
- configuration schema and digest.

Every call receives immutable `DecisionBudget`:

- absolute deadline and cancellation handle;
- optional CPU/memory/work-unit and candidate-cardinality limits;
- replay seed;
- request/attempt/candidate identity.

The application layer owns bounded `DecisionExecutor` instances at the place a
hook is invoked: APPDeployment owns deployment and deployment-time execution-
target execution; APPClient owns engine admission, model-variant/partition/
unified Provider assignment, request-DAG scheduling and request-time execution-
target execution; APPProvider owns provider admission, provider-local scheduling/
execution-tuning/cache/provider-target execution and
adapter creation. Recovery reuses the executor of the failed decision path.
Directly
registered, trusted extensions may run in process with cooperative cancellation.
Discovered third-party wheels default to an isolated worker process using the
versioned serialized contract.
In either mode, a result arriving after the deadline is irrevocably late and
cannot be applied by Core. Killing a worker process is an APP operation, not a
Core or network protocol function.

Worker isolation contains crashes, late work and configured resource use; it is
not a security sandbox. Digest/version allowlisting verifies the selected
installed identity but does not make arbitrary code trustworthy. Operators must
trust selected extension code, and the executor supplies only hook-required
inputs.

Every call produces typed output plus `OptimizationDecisionEvidence`:

- extension and contract identity;
- input/configuration/seed/output digests;
- start/end/duration;
- success, reject, timeout, cancelled, invalid, exception or fallback status;
- explicit fallback identity, when configured;
- human-readable explanation and machine-readable reason codes.

Calls use immutable batched decision epochs, never per-packet, per-segment or
per-generated-token callbacks. A proposal is reusable only while its input and
epoch digest remain current; Core/provider invalidation forces a new bounded
decision rather than applying stale policy output.

Objective metrics carry identity, unit, direction, aggregation/quantile,
normalization and missing-data rules. Forecasted or sampled facts use
`EstimateEnvelope` with observed/predicted/unknown kind, confidence, horizon,
source and freshness. Hard feasibility is evaluated before normalized
preference ranking; an extension cannot treat unknown as zero or mix raw units.

## Narrow policy interfaces

Each hook has one required method. Concrete naming may follow the host language,
but the semantic input/output is fixed.

| Policy | Immutable input | Proposed output | Non-overridable mechanism |
| --- | --- | --- | --- |
| `DeploymentPolicy.plan` | deployment/model residency, capacity, active lease/session facts, exact constraints, objective, snapshot projection and budget | idempotent use-existing/rank, activate, reserve, prewarm, scale-out/in or drain/unload proposal with lifecycle preconditions | identity, active leases/sessions, cooldown/residency/drain, exact constraints, capacity and reservation validity |
| `ModelVariantPolicy.propose` | authorized model alternatives, quality contract, objective, model/runtime snapshot projection and budget | bounded ranked/pruned `ModelVariantCandidateSet` or exact-constraint singleton evidence | exact constraint, alternative membership, quality declaration, semantic and artifact/tokenizer identity |
| `PartitionPlanner.plan` | model-variant candidates, provider capability snapshot, objectives, budget | immutable variant-bound `PlanCandidateSet` with explanations | Core plan/schema/dependency validation and candidate bounds |
| `ProviderAssignmentPolicy.assign` | variant-bound plans, bounded role/Provider target candidates, successful eligible ACK/capability candidates, multiplicity, objectives and budget | selected variant + plan, Provider assignments + role-target IDs and ranking | authorized variant/plan/target binding, ACK membership, token/permission, freshness, feasibility, leases, authority and multiplicity |
| `SchedulingPolicy.dispatch` | declared `REQUEST_DAG` or `PROVIDER_LOCAL` scope, admitted dependency-ready work, adapter compatibility, capacity, objective and hard bounds | scope-valid DAG work or compatible local dynamic batch/order, phase/fairness/preemption/assignment-authorized hedge directives plus bounded grants | scope, readiness, adapter batch capability, queue/assignment membership, deadline and mechanism hard bounds |
| `ExecutionTuningPolicy.tune` | dispatched invocation, typed `TuningParameterSpec`, workload facts, snapshot projection and bounds | declared segment/microbatch/compression/transfer/prefetch/overlap/speculative-window values | declaration, type/range/scope, memory, deadline and provider safety limits |
| `CachePolicy.decide` | phase-tagged eligible exact/semantic/prefix/KV actions and immutable state | closed lookup/reuse/place/prefetch/admit/retain/evict/migrate action plus affinity, never final compute Provider | action/phase, key/security/epoch validity, capacity and atomic update |
| `AdmissionPolicy.decide` | declared `ENGINE_REQUEST` or `PROVIDER_LOCAL` scope, request plus telemetry and mandatory safety floor | scope-valid accept/defer/reject proposal | prior-scope rejection and mandatory floor; extension may be stricter, never weaker |
| `RecoveryPolicy.decide` | Core-allowed transitions/checkpoint boundaries, failure/progress facts, remaining deadline/attempt budget | retry-same/resume/restart/reassign/repartition/redeploy/defer/fail directive without embedded replacement | attempt/deadline/output-commit bounds, exclusions and stale/duplicate-result rejection |
| `ExecutionTargetPolicy.propose` | plan-role-Provider alternatives, compatible registered runtime/engine/adapter/device alternatives and advertised capabilities | bounded target candidates per plan-role-Provider plus explicit fallbacks | adapter registration, artifact/device/batch/checkpoint compatibility, candidate bounds and actual availability |

Planner registry lookup is explicit configuration and is not accepted as an
`ExecutionTargetPolicy` decision. Recovery directives that require a new
assignment, partition or deployment invoke the owning policy after validation.
Cache affinity becomes Provider-assignment input rather than a competing final
compute-Provider choice.

## Runner adapter SPI

| SPI | Immutable input | Created output | Non-overridable mechanism |
| --- | --- | --- | --- |
| `RunnerAdapter.create` | validated selected target plus model/artifact descriptor and advertised `BatchCapability` | Python inference handler or native runner factory registration | selected adapter identity, artifact/device/capability validation, provider permission, evidence and response authority |

`ExecutionAdapterRegistry` is instance-scoped and independent of
`OptimizationRegistry`. Registration is not selection. Only a target proposed
by `ExecutionTargetPolicy`, selected with Provider assignment and validated in
the committed intent may be resolved and created.

Unknown fields are rejected unless the negotiated contract version explicitly
permits forward-compatible metadata. Extensions do not receive prompt/tensor
payloads by default, tokens, private keys, decrypted policy material or
authority-changing handles.

## Outcome observer SPI

`OptimizationObserver` is optional and independent of `OptimizationSuite` and
`ExecutionAdapterRegistry`:

| SPI | Immutable input | Effect | Non-overridable mechanism |
| --- | --- | --- | --- |
| `OptimizationObserver.observe` | bounded `OptimizationOutcome` with decision/intent/execution/candidate lineage, standardized metric envelopes and sensitive payloads removed | observer-owned offline learning, evaluation or audit state update | current inference result, Core availability, evidence identity and policy-state lineage |

Observation runs after commit/completion/failure on a separately budgeted APP
executor, is idempotent by outcome identity and cannot synchronously alter the
current request. Stateful extensions record the `policy_state_epoch` and
`policy_state_digest` read by each decision. Persistence and concurrent update
semantics belong to APP/extension; Core supplies no plugin state store.

## Registration and selection

Direct registration is mandatory:

```text
registry = OptimizationRegistry()
registry.register(suite)
adapters = ExecutionAdapterRegistry()
adapters.register(team_x_runner)
app = APPClient(...,
                optimization_suite=registry.require("team-x", "1.2.0"),
                execution_adapters=adapters)
```

Registration is scoped to that APP/provider instance. Duplicate identities,
incompatible contract versions and missing policies/adapters fail before
inference.

An omitted hook resolves to the corresponding named member of
`DefaultOptimizationSuite` through the same registry/executor/evidence path.
The engine records a model-variant hook as not applicable rather than invoking
it when one exact model constraint is present.
Omission never calls a private legacy branch. A selected hook failure does not
fall back to that default unless the default is separately configured as the
explicit fallback.

Optional discovery requires all of:

1. an explicit APP/operator request to discover;
2. entry-point name;
3. allowlisted distribution name and semantic-version constraint;
4. verified installed-distribution/source digest;
5. explicit suite selection after discovery.

There is no import-time discovery, module-global default registry, environment
variable selection or Core-owned loader.

Contract compatibility is checked before activation. An incompatible contract
major version is rejected. Forward-compatible metadata is accepted only when
the current Core contract explicitly declares that field namespace extensible;
otherwise unknown fields fail validation.

## Failure and fallback

Before side-effecting execution, APP assembles one `ValidatedExecutionIntent`
that binds selected model/plan/assignment/target/tuning/cache-affinity plus
objective, snapshot and policy-state lineage. Core/provider mechanisms prepare,
revalidate, acquire reservations and atomically commit or abort the whole intent.
Abort releases acquired resources; no policy may directly commit partial state.

- Timeout, cancellation, exception, malformed output, version mismatch,
  unavailable dependency and non-reproducible result fail closed.
- A late in-process result is discarded even if its Python thread cannot be
  forcefully stopped; deployment mode uses worker-process isolation when hard
  termination/resource containment is required.
- Fallback is disabled by default.
- APP may name one fallback per hook. Its identity and the triggering failure
  are included in decision evidence.
- Partial extension output is never merged with a fallback result.
- Core/provider validation occurs again after fallback.

## Reproducibility

- Deterministic extensions MUST return the same output digest for the same
  contract version, immutable input, configuration and seed.
- Stochastic extensions MUST consume the supplied seed and reproduce when that
  seed is replayed.
- A package that cannot meet replay requirements may be used only as an
  exploratory adapter; its decisions cannot be promoted to deployment or
  candidate-bound research evidence.

## Native runner integration

The public C++ SDK retains `NativeModelRunner`, `NativeModelRunnerFactory`,
`RegistryNativeModelRunnerFactory`, `NativeProviderRuntime` and `DependencyIo`.
An out-of-tree adapter includes installed public headers, links the public SDK,
registers its backend factory, and links into its provider executable. It does
not edit NDNSF-DI source or rely on private headers.

Spec 111 does not promise a stable binary ABI across compilers/standard-library
versions and does not load arbitrary shared objects at runtime. Source/API
compatibility and clean out-of-tree build validation are the acceptance target.

## Contract-test kit

The reusable kit validates:

- descriptor, version and capability consistency;
- immutable input/no global mutation;
- output schema and stable reason codes;
- timeout, cancellation, exception and missing dependency behavior;
- malicious/stale/unknown proposal rejection by Core/provider mechanism;
- same-input/configuration/seed replay;
- 100 concurrent calls across two suite instances with zero state bleed;
- explicit fallback evidence;
- public-import-only and clean-wheel installation;
- all ten Python policies replace their intended decision in focused fixtures;
- partial suites resolve omitted policies to evidenced named defaults;
- Runner registration alone never selects a runner, while a changed validated
  target adapter identity changes creation through the instance registry;
- scoped engine/provider admission and request-DAG/provider-local scheduling,
  including adapter-declared mixed-phase versus homogeneous batching;
- joint model/plan/assignment selection and deterministic candidate-budget
  pruning without premature model commitment;
- atomic execution-intent prepare/revalidate/commit/abort fault injection;
- streaming checkpoint/output-commit recovery with no duplicate visible output;
- observer idempotency, state epoch/digest replay and observer-failure isolation;
- least-input tests with zero prompt/tensor/credential/cross-tenant disclosure;
- the decision-point inventory has no unclassified or native-only policy site;
- frozen default-suite fixtures reproduce characterized current decisions.
