# Implementation Plan: NDNSF-DI Core/APP Separation

**Branch**: `Experimental` | **Date**: 2026-07-14 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/111-ndnsf-di-core-app-separation/spec.md`

## Summary

Extract a workload-neutral NDNSF-DI execution Core from the current mixed Python
distribution without rewriting the native runtime or changing top-level
wire/security authority. First freeze behavior and inventory compatibility surfaces; then move
contracts and mechanisms behind owner-specific modules, introduce an
application-owned process-local `DistributedInferenceEngine`, complete Python
optimization SDK, shared objective/snapshot contracts and immutable request-scoped
assignment context, bind distributed execution to authenticated lease receipts,
attempt/deployment fencing and an immutable commit certificate, expose a
definition -> immutable revision -> apply/readiness -> durable request -> drain
operator/application workflow, define the operations-owned OCI-to-SIF/Slurm
handoff without making APPDeployment an infrastructure scheduler, migrate every current decision algorithm into a versioned
default suite, migrate APP/planner/model/ops callers, and only then split
installation profiles and retire bounded compatibility adapters. Spec
107/109/110 evidence is read-only lineage; every post-separation execution uses
a new candidate identity.

## Technical Context

**Language/Version**: Python 3.8+ for orchestration/contracts; C++17 native DI and NDNSF runtime retained

**Primary Dependencies**: Existing `ndnsf` Python binding, native NDNSF-DI library, ndn-cxx/NFD/ndn-svs, optional ONNX Runtime, Qwen/llama adapters, setuptools/container packaging

**Storage**: Existing JSON/YAML plans/deployment profiles, compatibility manifest, evidence files and external model artifacts plus one fixed APP-owned append-only filesystem `RuntimeJournal` under an operator-mounted persistent state root; no new cluster database or public state-store SPI

**Testing**: Python `unittest`, C++ Boost.Test, import/dependency contract checks,
concurrency regressions, local package builds, existing security scripts,
MiniNDN-only distributed/security/fault/operational/performance gates, and static
Spec-111-to-Slurm handoff rendering; no OCI/SIF build or container runtime

**Target Platform**: Ubuntu 20.04/22.04 local and MiniNDN environments only;
OCI/Apptainer/iTiger execution is deferred to Spec 110

**Project Type**: Multi-language library, application SDK, optional model adapters, CLI, and deployment packaging

**Performance Goals**: Zero correctness/completion regression; a predeclared ten-pair 60-second MiniNDN campaign keeps median paired latency/throughput and its 95% bootstrap interval within the 5% non-regression margin before compatibility deletion

**Constraints**: Behavior-preserving first; no new top-level wire name, security authority or consensus service; versioned extensions to existing lease/Selection/result payloads are permitted only for the consistency contract; no global placement environment mutation; no historical evidence relabeling; no new planner service/network coordinator; exact model constraints cannot be replaced; no second deployment manifest or CLI lifecycle implementation; NFD/ServiceController remain externally supervised; Docker is not an iTiger runtime and Slurm status is not APP readiness; Spec 111 performs no OCI/SIF build, container-runtime execution or Slurm/iTiger submission; no large-scale SDK implementation before revised structure/analysis/audit PASS and the T064 distributed-consistency gate; preserve negative results

**Scale/Scope**: 17,616 Python lines in the current package, 174 root exports, 3635-line `runtime_v1.py`, native C++ execution path, existing examples/tests/containers and Spec 107-110 callers

## Constitution Check

*GATE: PASS before research; re-checked after design.*

| Principle | Design response | Gate |
| --- | --- | --- |
| Canonical Dynamic Runtime | No generated/static service path, split service names, or Direct terminology is introduced | PASS |
| Security Is Part Of Data Path | NAC-ABE, permissions, tokens, replay, leases and provider authority remain Core-enforced invariants | PASS |
| CodeGraph First | Current symbols, imports, callers and tests were inspected before planning; index is current | PASS |
| Spec-Driven Durable Work | Spec 111 owns requirements, contracts, tasks, migration and evidence gates | PASS |
| Verify With Right Scope | Characterization, unit/contract/security, concurrency, MiniNDN and container gates are proportional to risk | PASS |

Additional project gates:

- Spec 110 remains the concrete iTiger owner. Spec 111 defines only its immutable
  runtime handoff and may append linked remediation tasks; no prior evidence,
  candidate or completed task is rewritten or relabeled.
- Python planning remains outside the native hot path.
- Application-specific model, cache and scheduling policy stays outside NDNSF Core.
- GSD health is degraded only by an unrelated stale Spec 110 worktree; Spec 111
  does not remove it.

## Architecture and Ownership

### Accepted dependency direction

```text
applications / experiments / deployment adapters
                 |
                 v
      APP SDK / DistributedInferenceEngine
                 |
                 +------> planner + placement policies
                 |                    |
                 v                    v
              DI Core <-------- model runner adapters
                 |
                 v
             NDNSF Core
                 |
                 v
          ndn-cxx / NFD / ndn-svs
```

Forbidden directions:

```text
DI Core -> APP SDK
DI Core -> planner implementation
DI Core -> ONNX/Qwen/llama implementation
DI Core -> GUI / Experiments / operations implementation
NDNSF Core -> any DI-specific model or planner semantic
```

### Owner matrix

| Concern | Owner | Core relationship |
| --- | --- | --- |
| Plan/assignment schema, atomic intent commit/abort, dependency execution, progress/output epoch, cache binding, recovery evidence | DI Core | Direct implementation |
| Candidate eligibility, telemetry freshness, leases, authority validation | DI Core using NDNSF Core facts | Non-overridable invariant |
| Deployment, split, unified ordinary/multi-role Provider assignment and SLO objectives | Python optimization SDK / Planner / APP | Injected typed decision |
| Optional quality-constrained model-variant candidates | Python optimization SDK / Planner / APP | Ranks/prunes only APP/operator-authorized alternatives; assignment selects final variant/plan tuple |
| Scoped ready-work dispatch, execution tuning, cache, admission, recovery and execution-target objectives | Python optimization SDK / Planner / APP | Injected typed decision constrained by scope, adapter capability and Core/provider safety |
| Process-local decision graph, objective/snapshot projection and policy invocation | APP-owned `DistributedInferenceEngine` | Composition only; Core validates and executes; never a network coordinator |
| Optimization registration/discovery and decision budgets | APP SDK | Instance-scoped registry and allowlisted loader |
| Outcome observation and optimizer state | APP SDK / external extension | Optional post-result SPI; state/persistence never owned by Core |
| Cache residency/state correctness | DI Core | Binding, digest, epoch, eviction safety |
| Cache placement objective and reuse preference | Planner/APP | Policy over Core facts |
| APPClient/APPProvider/APPDeployment/APPController convenience | APP SDK | Calls DI Core |
| ONNX/Qwen/llama parsing, partition and runner implementation | Model adapter | Implements planner/runner contracts |
| CLI, doctor, status, metrics, benchmark launch | Operations | Calls owner interfaces |
| NDN security, V2 invocation, discovery facts, generic leases/telemetry | NDNSF Core | Canonical mechanism; only versioned receipt/certificate evidence extensions are allowed |
| Per-request prepare/commit/abort coordination and commit-certificate assembly | Initiating APPClient identity | Coordinates existing provider-local lease mechanisms; gains no provider capacity or security authority |
| Provider boot epoch, lease receipt, activation and orphan reclamation | Provider / NDNSF Core lease table | Authoritative only for local resources; rejects stale attempt, certificate and boot epochs |
| Deployment lifecycle action stream and fencing | Operator-authorized APPDeployment identity | Single writer per deployment ID/lifecycle epoch; Providers enforce compare-and-set and action certificates |
| Definition validation, immutable revision, apply/status/wait/rollback/drain/delete and reconciliation | APPDeployment | Uses existing deployment configuration, deployment consistency and fixed RuntimeJournal; never implemented in CLI/Core |
| Request submit/open/status/wait/result/cancel/stream | APPClient | Uses one durable request handle over Core certificate/rendezvous; Future/synchronous APIs are adapters |
| Persistent lifecycle/request evidence | APP-owned fixed RuntimeJournal plus protected request-envelope spool/reference | Local mounted filesystem or durable NDN envelope repository by default; no plaintext/secrets, optimizer state or cluster-database authority |
| Revision-bound staging/readiness/drain/local cleanup | APPProvider | Verifies external artifacts, adapter/runtime, permissions, boot/revision epoch and active bindings |
| Command rendering and status/events/metrics presentation | Operations | Thin calls to public APP APIs with no lifecycle/inference decision logic |
| OCI/SIF/Slurm infrastructure allocation and process launch | Existing operations runtime adapter / Spec 110 | Produces a distinct allocation handle and launches generic agents; never becomes APP/Core policy or equates RUNNING with READY |
| MiniNDN/iTiger campaigns | Experiments/deployment specs | Validate, never imported by Core |

## Design Phases

### Phase A - Freeze and Inventory

1. Capture root imports, command entry points, config/schema keys, C++ targets,
   examples, tests, packaging and experiment callers in a compatibility manifest.
2. Add characterization tests before moving implementation.
3. Record the frozen candidate/source identity and historical evidence digests.
4. Establish dependency-direction tests that initially report current debt and
   become blocking only as each owner migration is completed.

No implementation is deleted in this phase.

### Phase B - Extract a Deep DI Core

Move behavior without redesign into owner-focused modules:

```text
NDNSF-DistributedInference/ndnsf_distributed_inference/
├── core/
│   ├── contracts.py          # plan, assignment, evidence, state identities
│   ├── eligibility.py        # non-overridable candidate validation
│   ├── execution.py          # orchestration over native runtime
│   ├── recovery.py           # bounded replan/recovery mechanism
│   ├── state.py              # cache/session/attempt bindings
│   ├── ports.py              # mechanism-facing decision request/result ports
│   └── placement.py          # eligibility/decision validation/application only
├── sdk/
│   ├── contracts.py          # public typed optimization hook contracts
│   ├── registry.py           # instance-scoped explicit registry
│   ├── loader.py             # opt-in allowlisted entry-point loading
│   ├── suite.py              # ten-policy OptimizationSuite composition
│   ├── adapters.py           # ExecutionAdapterRegistry + RunnerAdapter SPI
│   └── contract_tests.py     # reusable external-package conformance kit
├── app_sdk/
│   ├── contracts.py          # revisions, lifecycle operations, request handles
│   ├── client.py
│   ├── provider.py
│   ├── deployment.py
│   ├── runtime_journal.py    # fixed local durable recovery mechanism
│   ├── status.py             # stable lifecycle/request status and events
│   ├── engine.py             # process-local decision-DAG composition root
│   ├── controller.py
│   ├── policy.py
│   └── gui.py
├── planner/
│   ├── registry.py
│   ├── model_variant_policy.py
│   ├── cost_policy.py
│   ├── split_policy.py
│   └── defaults.py           # default suite from installed owner defaults
├── adapters/
│   ├── onnx/
│   ├── qwen/
│   └── llama/
├── ops/
│   ├── cli.py
│   └── contract_smoke.py
└── compatibility/
    ├── exports.py
    └── manifest.json
```

The existing `runtime_v1.py`, `app.py` and package-root exports become thin
compatibility adapters during migration. They contain no duplicate scoring,
execution, authority, or state implementation.

Current `choose_cache_placement`, `proportional_layer_allocation`, local LLM plan
construction and context-sweep objectives move to planner/model owners. Runtime
contract smoke and simulation utilities move to operations/experiment owners;
they are not DI Core execution.

The C++ native path under `cpp/ndnsf-di/` is not moved in the first increment.
Its existing `NativeExecutionPlan`, `NativeProviderAssignment`,
`NativeProviderSession`, `NativeProviderRuntime`, `DependencyIo` and
`NativeModelRunnerFactory` are the reference execution seam.
Model-specific `OnnxRuntimeModelRunner` and `QwenGenerationSession` keep their
symbols/namespaces but move physically under adapter-owned C++ directories after
the native characterization gate. Core-only targets cannot compile or link
those sources.

### Phase B.5 - Lock Distributed Execution and Deployment Consistency

Before implementing the engine graph, preserve the existing
`GenericExecutionLease`, `ProviderExecutionLeaseTable`, authenticated Targeted
lease service, V2 request/Selection path and `ExecutionAttemptAuthority` as the
only execution-authority mechanisms. The initiating `APPClient` identity is the
coordinator for one `(requester, request_id, attempt_epoch)`; an
operator-authorized `APPDeployment` identity is the single writer for one
deployment lifecycle stream. Providers remain authoritative for their local
capacity, boot epoch, activation and cleanup.

The request coordinator prepares every Provider, revalidates and commits every
lease, then constructs an immutable `ExecutionCommitCertificate` from the full
set of authenticated commit receipts. Selection/assignment cannot activate work
without that complete certificate. Prepare and commit reserve resources but do
not execute them. This is atomic execution visibility, not a claim of
simultaneous global state transition, leader election, consensus or serializable
cluster storage.

Attempt epoch, Provider boot epoch, lease identity/expiry and deployment
lifecycle epoch form the fencing boundary. Requester crash/restart, network
partition, provider restart, duplicate/reordered messages and conflicting
deployment writers all fail closed for new authority. Bounded leases and
periodic provider cleanup reclaim prepared, committed and executing-orphan
resources even when no later request arrives, while bounded high-watermark
tombstones outlive resource release long enough to reject delayed stale
operations. The normative state machines,
receipt/certificate schemas and fault matrix are defined in
`contracts/distributed-execution-consistency.md`.

### Phase C - Publish the External Optimization SDK

The engine uses a validated decision DAG, not an undocumented fixed linear
chain. The normal dependency shape is:

```text
control/session epoch:
  OptimizationObjective + workload forecast + EngineSnapshot
    -> DeploymentPolicy -> idempotent lifecycle action -> new snapshot

request epoch:
 OptimizationObjective + lineage-bound EngineSnapshot
  -> AdmissionPolicy(ENGINE_REQUEST)
  -> ModelVariantPolicy proposes bounded candidates; exact singleton otherwise
  -> PartitionPlanner -> variant-bound PlanCandidateSet
  -> ExecutionTargetPolicy proposes targets per plan-role-Provider alternative
  -> CachePolicy LOOKUP/PREFETCH -> cache affinity
  -> ProviderAssignmentPolicy selects variant + plan + Providers + role targets
  -> prepare/revalidate/commit ValidatedExecutionIntent
  -> SchedulingPolicy(REQUEST_DAG) dispatches ready roles/transfers
  -> AdmissionPolicy(PROVIDER_LOCAL)
  -> SchedulingPolicy(PROVIDER_LOCAL) forms adapter-compatible batches/grants
  -> ExecutionTuningPolicy selects typed bounded invocation parameters
  -> ExecutionAdapterRegistry resolves RunnerAdapter -> Core/native execution
  -> Core progress/output-commit/checkpoint epochs
  -> CachePolicy ADMIT/RETAIN/EVICT/MIGRATE after validated state transition
  -> RecoveryPolicy may re-enter only its declared owning node within budget
  -> optional asynchronous OptimizationObserver receives OptimizationOutcome
```

Every node has an APP invocation owner, `DecisionBudget`, immutable least-input
projection, Core/provider validator, explicit invalidation trigger and decision
evidence. A snapshot epoch change invalidates dependent proposals rather than
silently combining facts. The graph is process-local composition: it creates no
NDN planner service, cluster coordinator, authority or persistence engine.
Deployment is not an unconditional request predecessor; no-feasible planning may
use only a declared bounded re-entry through the control/session epoch and a new
snapshot.

`FixedProviderAssignmentPolicy` proves deterministic operator/application
choice.
`CostProviderAssignmentPolicy` preserves the current ordinary/multi-role score
behavior as a migrated default, including its explicit versioned weights. The
SDK exposes ten narrow Python policy ports, normatively defined in
`contracts/python-optimization-surface.md`:

1. `DeploymentPolicy`;
2. `ModelVariantPolicy`;
3. `PartitionPlanner`;
4. `ProviderAssignmentPolicy`;
5. `SchedulingPolicy`;
6. `ExecutionTuningPolicy`;
7. `CachePolicy`;
8. `AdmissionPolicy`;
9. `RecoveryPolicy`;
10. `ExecutionTargetPolicy`.

`ModelVariantPolicy` is present in the public suite but applicable only when
APP/operator input supplies two or more semantically acceptable model variants
and an explicit quality metric/floor. It selects model identity/size, revision,
tokenizer, precision, quantization, optional draft model and adapter candidates
only by bounded rank/prune within that set. One exact model identity records
`NOT_APPLICABLE_EXACT_CONSTRAINT` and becomes a singleton. Partition plans bind
variant identities, target policy proposes compatible plan-role-Provider
targets, and Provider assignment selects the final authorized
variant/plan/Provider/target tuple.

`RunnerAdapter` is a separate factory SPI registered in an instance-scoped
`ExecutionAdapterRegistry`; it is not an optimization policy and is not a field
of `OptimizationSuite`. `ExecutionTargetPolicy` proposes compatible target
candidates; Provider assignment selects their IDs, after which the registry
creates the runner from the committed intent. Optional `OptimizationObserver` is a second independent
non-policy SPI for post-outcome feedback; it cannot affect the current request.

Ordinary ACK selection and multi-role placement share
`ProviderAssignmentPolicy`. One-role requests use a synthetic role and
multiplicity; multi-role requests use explicit plan roles. `PartitionPlanner`
returns variant-bound `PlanCandidateSet` entries, and Provider assignment
returns the selected variant/plan/target identities together with Provider
assignments, allowing joint model/partition/Provider/target scoring without a
universal untyped policy.

`SchedulingPolicy.dispatch` is one family with two non-overlapping scopes.
APPClient owns `REQUEST_DAG` role/transfer ordering and assignment-authorized
hedges. APPProvider owns `PROVIDER_LOCAL` continuous/dynamic batch membership,
phase arbitration, fairness/priority, bounded preemption/resume and worker
grants. Batch compatibility is advertised by the Runner adapter; mixed
prefill/decode is allowed only when that capability is explicit.
Post-failure transitions are never scheduling decisions.
`ExecutionTuningPolicy.tune` owns only declared typed/ranged per-invocation
parameters such as segment/microbatch, compression, transfer chunk, prefetch
depth, compute/communication overlap and speculative-decoding window.
`CachePolicy` uses closed LOOKUP/REUSE/PLACE/PREFETCH/ADMIT/RETAIN/EVICT/
MIGRATE_OR_REPLICATE action kinds, separates pre-assignment affinity from
post-execution mutation epochs, and never makes the final compute Provider
assignment. `RecoveryPolicy` selects a transition and delegates any
new assignment, partition or deployment to its owning policy. Planner registry
lookup remains explicit configuration, while `ExecutionTargetPolicy` owns only
runtime/engine/device/fallback choice.

`DeploymentPolicy` is a deployment/session control-plane hook. Its action set
is USE_EXISTING/RANK, ACTIVATE, RESERVE, PREWARM, SCALE_OUT, SCALE_IN and
UNLOAD_OR_EVICT. It is not invoked per token or unconditionally per request;
active leases/sessions, capacity, artifact identity and exact constraints block
unsafe lifecycle changes. Actions are idempotent and carry lifecycle epoch,
readiness, minimum-residency, cooldown/hysteresis and bounded-drain preconditions.

The shared `OptimizationObjective` separates hard constraints from weighted
preferences and standardizes deadline, TTFT, TPOT, throughput, quality metric
and floor, cost, energy, fairness/priority and availability/recovery intent.
Each metric declares unit, direction, aggregation/quantile, normalization and
missing-data behavior; hard feasibility precedes preference ranking.
`EngineSnapshot` binds model/deployment residency, Provider capability,
queue/worker state, GPU compute/memory, link RTT/bandwidth/loss, cache/KV/prefix
state, runtime/adapter compatibility, workload facts, timestamps and lineage.
Observed/predicted facts use `EstimateEnvelope` with confidence, horizon, source
and freshness. Policy projections exclude prompt/tensor payload and secrets.
Each hook receives only its least-input projection. A shared estimator may be
an implementation detail inside one suite; Spec 111 does not add a separate
`CostModel` policy/SPI without evidence of an independent interoperability need.

The machine-readable
`specs/111-ndnsf-di-core-app-separation/contracts/decision-point-inventory.json`
maps every current production
score/rank/select/order/plan/tuning site to one port, invocation owner, Core or
provider validator, versioned default and acceptance evidence. Static tests fail
if a new policy-bearing source site is not classified. Mechanism-only decisions
must be explicitly recorded with the invariant that makes them non-overridable.

Core owns the minimal immutable mechanism-facing port types in `core/ports.py`.
The SDK re-exports and adapts those exact contracts, so Core never imports SDK
code and there is no duplicate schema. All Python extension protocols require
at most one decision/creation method per hook. They share
`OptimizationExtensionDescriptor`, `DecisionBudget` and
`OptimizationDecisionEvidence`. Algorithms may be deterministic or stochastic;
stochastic results must record a seed and reproduce under the same input,
configuration and seed before they can become deployable evidence.

All current algorithms implement the same public ports as an external package.
Planner-owned defaults compose into `DefaultOptimizationSuite`; model-specific
default implementations are registered explicitly by their installed adapter
wheel, so SDK/Core never import Planner or optional adapters. Missing hooks
explicitly select a named installed default and emit normal decision evidence;
they never call a privileged legacy branch. A selected hook failure remains
fail-closed unless a fallback is separately configured, even when that fallback
is a default policy.

`OptimizationRegistry` is constructed per APP instance. Direct object
registration is the baseline. Entry-point discovery under
`ndnsf_di.optimizers` is optional, APP-owned and disabled unless the operator
allowlists distribution name, version and digest. Core never imports or
discovers third-party code. Timeout, exception, invalid output and dependency
failure produce typed evidence; fallback occurs only when the APP explicitly
names it.

The process-local APP-owned `DistributedInferenceEngine` is the composition root
for one immutable `OptimizationSuite`, `ExecutionAdapterRegistry`, objective,
snapshot lineage and decision graph. It delegates each node to the existing
application-layer `DecisionExecutor` at its invocation site: APPDeployment for
deployment; APPClient for engine admission, request-time model candidates,
partition, unified Provider assignment and request-DAG scheduling; execution-
target selection runs under the APP owner that has the alternatives; APPProvider
owns provider admission, provider-local scheduling, execution tuning, cache and
adapter creation; recovery reuses the failed-path owner.

Before side effects, APP prepares one lineage-bound `ValidatedExecutionIntent`.
Core/provider mechanisms revalidate all decisions/reservations together, reuse
the canonical lease transaction and admit execution only after a complete
authenticated commit certificate; abort releases acquired resources. Streaming
progress/output/checkpoint epochs bound recovery. Completed/rejected/failed
attempts emit `OptimizationOutcome`; optional observation runs off the critical
path. Stateful extensions own persistence/concurrency and record policy-state
epoch/digest; Core provides no plugin database.
Trusted direct objects may use bounded in-process cooperative
execution; discovered third-party wheels default to a worker process over the
versioned serialized contracts. Core rejects every late result regardless of
whether an in-process thread has returned. This provides hard-termination and
resource-containment options without turning optimization into a network
service or placing plugin lifecycle inside Core. It is not a security sandbox;
allowlisting verifies installed identity and operators must trust selected code.

The existing built-in ACK selectors, `PlannerBackendRegistry`, APP deployment
ordering, `APPProvider` handler/admission path, Runtime v1 scoring/scheduling/
resource/cache/recovery functions, semantic cache policies, backend selectors
and native runner factory become default adapters/reference implementations of
this SDK. They are not parallel sources of truth.

### Phase D - Remove Process-Global Assignment State

`AssignmentContext` is immutable and request-scoped. It includes request,
template, policy, snapshot, role assignments, lease bindings, deadline,
attempt/epoch and evidence lineage. The compatibility helper converts legacy
deployment records to this context and passes it explicitly. Tests forbid writes
to `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE`.

### Phase D.5 - Close the Deploy-and-Use Workflow

Extend the existing deployment configuration into a validated
`DeploymentDefinition` and immutable `DeploymentRevision`; do not introduce a
second manifest. `APPDeployment` owns validate/resolve/dry-run/apply/status/
wait/rollback/drain/delete and persists lifecycle/action evidence in the fixed
APP-owned `RuntimeJournal`. `APPProvider` stages external artifacts and publishes
revision-bound readiness. `APPClient.submit()` returns a durable
`InferenceRequestHandle`; synchronous and Future APIs delegate to it.

The default journal is an append-only versioned filesystem mechanism under a
persistent mounted state root. It stores digests, receipts, certificates,
rendezvous pointers and fencing/rollback evidence, not weights, secrets or raw
payload. It is not an external optimization SPI or cluster database. CLI/status
code remains a thin presentation adapter over APP operations. Durable submit
persists the existing authenticated/confidentiality-protected request wire
envelope in an owner-only adjacent spool or durable NDN repository and journals
only its digest-bound reference; it never reconstructs missing input.

Definition/revision state, running instance phase and reason-coded health
conditions remain distinct. External container/Slurm/systemd/operator layers
start generic Provider agents; apply selects agents and performs revision-scoped
stage/warm/readiness. APPDeployment does not provision OS processes.

The complete supported path is validate -> resolve -> dry-run -> apply -> stage/
warm -> ready/active -> submit -> certified result -> reopen/recover -> drain ->
inactive/delete. Upgrade creates a new revision; rollback applies an old revision
under a new lifecycle epoch. `deploy_plan()` is retained only as a compatibility
alias for metadata-only `prepare_session()`.

### Phase D.6 - Define the iTiger Runtime-Adapter Handoff

Keep Docker/OCI as the sealed build source and Apptainer SIF as the iTiger
runtime. The existing operations-owned Slurm adapter maps one immutable Spec 111
candidate/revision into a distinct allocation handle and a revision-derived
process map. It launches one containerized NFD per node, controller, generic
Provider agents and an in-allocation APP coordinator/client; APPDeployment then
owns revision stage/warm/READY/ACTIVE/drain inside those live agents.

The handoff binds exact OCI/SIF/model/artifact/process-map/network/state/identity
digests. Release/model/artifact/identity binds are read-only, the identity-
partitioned RuntimeJournal root is persistently read-write, scratch/node-run
paths are allocation-local read-write, and broad `/project` or host project
executables are forbidden. Slurm and APP/request states remain separate.

Spec 111 closes only static/offline contract and migration evidence; it does not
build an OCI image, materialize/run a SIF or invoke a container runtime. Spec 110
owns the first post-separation OCI build/publication, SIF materialization,
Apptainer runtime, GPU/network probes and live Qwen jobs under fresh
authorization. The complete boundary is normative in
`contracts/itiger-slurm-apptainer-handoff.md`.

### Phase E - Isolate Installation and Runtime Profiles

Owner-specific profiles are produced from the same source tree:

- `core`: contracts, native execution bindings and required NDNSF dependencies;
- `sdk`: external optimization contracts, registry/loader/executor and test kit;
- `app`: APP SDK plus Core;
- `planner`: planner interfaces/implementations plus Core contracts;
- `model-onnx`, `model-qwen`, `model-llama`: optional adapters;
- `ops`: operator CLI and deployment helpers;
- `compat`: aggregate transition profile matching the old installation.

The build MUST produce separate installable artifacts before feature completion:

| Distribution | Owned package files |
| --- | --- |
| `ndnsf-di-core` | `ndnsf_distributed_inference/core/**` and installed public native Core headers/libraries |
| `ndnsf-di-sdk` | `ndnsf_distributed_inference/sdk/**` |
| `ndnsf-di-app` | `ndnsf_distributed_inference/app_sdk/**` |
| `ndnsf-di-planner` | `ndnsf_distributed_inference/planner/**` |
| `ndnsf-di-adapter-{onnx,qwen,llama}` | the matching Python/C++ adapter files |
| `ndnsf-di-ops` | `ndnsf_distributed_inference/ops/**` and canonical operator entry points |
| `ndnsf-distributed-inference` | compatibility-only root `__init__.py`, legacy modules and legacy entry-point delegations |

Owner wheels install disjoint subpackages under a PEP 420-compatible
`ndnsf_distributed_inference` namespace and do not own the root `__init__.py`.
Only the compatibility wheel owns the legacy root file/modules. The aggregate
depends on owner wheels and adds mappings, never duplicate implementations.
Clean build/install/uninstall tests compare wheel `RECORD` ownership and prove
that removing an optional owner does not delete another distribution's files.
Core-only and SDK-only module/SBOM inventories must not contain owner-forbidden
implementations.

The Python acceptance fixture is a standalone wheel under test fixtures and is
installed as an external distribution, not imported through the repository
path. A native sample is built out of tree against installed public headers and
links its factory into a provider executable; arbitrary `dlopen` and a stable
cross-compiler shared-library ABI are not introduced here.

### Phase F - Migrate, Measure, and Retire Compatibility

1. Migrate in-repository callers owner by owner.
2. Run security/native/APP and static container-contract regressions after every
   owner migration without building or starting a container.
3. Run the predeclared ten-pair 60-second MiniNDN non-regression campaign before deleting compatibility code.
4. Create a new candidate identity for post-refactor local/MiniNDN evidence and
   hand its source/offline-gate identity to Spec 110 for any later iTiger evidence.
5. Retire a compatibility mapping only after two consecutive inventories show
   zero repository callers, documented external migration gates pass, and
   rollback remains available.

## Security and Distributed Correctness

- Provider assignment policy is advisory and cannot bypass Core eligibility or
  authority.
- Deployment, model variant, Provider assignment, scheduling, execution tuning,
  cache, admission, recovery and execution-target hooks receive only Core/provider
  actionable sets, exact constraints
  or bounded ranges and cannot create readiness, validity, permission or budget.
- Third-party Python loading is opt-in, allowlisted and APP-owned; loading code
  is equivalent to executing application code and never occurs during Core import.
- Allowlisting is identity verification, not code sandboxing; operator trust and
  least-input disclosure remain explicit.
- Extension exceptions, timeouts and invalid decisions fail closed; fallback
  identity is explicit in evidence and is never silently substituted.
- Candidate snapshots and decisions are immutable, digest-bound and time-bound.
- Lease, token, replay, NAC-ABE, provider permission, attempt-epoch and stale
  result checks remain in their current authority-owning runtime.
- Assignment context is never accepted from ambient process state.
- Compatibility adapters translate inputs only; they do not maintain a second
  lease, security, assignment, cache, or recovery state machine.
- Malformed policy results fail closed with typed reason evidence.
- Mixed old/new callers converge on the same Core implementation.
- The engine is process-local composition and never receives security authority,
  private keys/tokens, a cluster-global singleton or an NDN coordinator name.
- Exact model/operator constraints dominate quality/cost weights; a model
  alternative outside the authorized set is rejected before partition/deploy.
- Dynamic batches require adapter-declared model/artifact/state compatibility;
  token phases may mix only when advertised. Active lease/session and cooldown/
  drain checks protect scale-in/unload/cache mutation.
- Policies receive workload shape rather than prompt/tensor/secret payload by
  default; cache and outcome projections remain tenant/security scoped.
- APP-prepared execution decisions become authoritative only through atomic
  execution visibility backed by a complete authenticated commit certificate;
  abort releases partial reservations and lease expiry bounds unreachable ones.
- Requester and deployment authority is fenced by attempt, Provider boot and
  lifecycle epochs; partitions cannot create new authority or permit an
  incomplete receipt set to execute.
- Provider cleanup runs periodically as well as on operation entry so orphaned
  reservations, sessions, cache pins and execution handles cannot persist
  indefinitely during idle periods.
- Deployment definitions, immutable revisions and runtime journals contain no
  weights, private keys, credentials, tokens or raw request payload.
- READY requires revision-bound role/artifact/adapter/permission/capacity facts;
  liveness, stale status or a metadata-only plan session never implies ACTIVE.
- Journal corruption, lock/version/quota failure and non-persistent deployment
  configuration fail closed for new lifecycle/request authority.
- NFD and ServiceController remain externally supervised; APP and CLI diagnose
  them but do not create an undeclared process-control authority.
- Request cancellation, upgrade, rollback, drain and delete preserve attempt/
  revision fencing, terminal evidence and shared external artifacts.
- Observer/state failure is isolated from current inference and Core availability.

## Migration and Rollback

| Phase | Forward action | Rollback surface | Deletion allowed? |
| --- | --- | --- | --- |
| A | Add inventory/tests | Remove new tests/docs | No |
| B | Move implementation + re-export | Restore previous import target | No |
| B.5 | Reuse leases and add coordinator/receipt/certificate/fencing contracts | Disable certificate-gated activation and restore the previous tagged lease behavior before any Engine caller migrates | No |
| C | Add engine/objective/snapshot/ten-policy seams preserving old decisions | Select migrated named defaults through the same engine graph | No |
| D | Pass explicit AssignmentContext | Compatibility translator to same Core | Old env write deleted only after concurrent gate |
| D.5 | Add immutable revision, RuntimeJournal, APP lifecycle/request handles and end-to-end deploy/use workflow | Compatibility APP façades plus previous validated revision/journal reader; no active revision is deleted | No |
| D.6 | Emit immutable runtime-allocation handoff and adapt operations callers without live submission | Retain the prior Spec 110 adapter/profile and mark post-Spec-111 iTiger unavailable; no candidate is relabeled | No |
| E | Build isolated profiles | Install compatibility aggregate | No owner implementation deletion yet |
| F | Migrate callers and remove shims | Previous tagged compatibility release | Yes, only after exit gate |

Persisted JSON/YAML/wire schemas remain backward-readable. Consistency fields
may extend only the existing versioned lease, Selection and result payloads;
unknown versions fail closed and mixed-version activation is disabled. Any
other schema or top-level name change requires a separate feature revision.

## Validation Strategy

### Static and contract gates

- import graph forbids reverse dependencies and optional-dependency leakage;
- compatibility manifest covers every exported/imported/build/CLI surface;
- decision-point inventory covers every production choice/score/rank/order/plan/
  tuning site and rejects unclassified or native-only policy branches;
- root re-exports resolve to one canonical owner;
- legacy environment preference has zero writers;
- Core-only installation/module inventory contains no forbidden owners;
- deployment configuration resolves to one canonical revision and
  `deploy_plan()` has no Provider/lifecycle mutation path;
- operations CLI imports/calls public APP APIs and contains no lifecycle,
  readiness, revision-selection or inference decision implementation.
- iTiger handoff binds exact candidate/revision/OCI/SIF/process-map/model/
  identity/state digests; static scans reject Docker-daemon use, broad writable
  project binds and scheduler-state-as-APP-readiness logic.

### Focused behavior gates

- current runtime-aware planner and placement tests;
- fixed versus cost policy deterministic selection;
- all ten external Python policies mutate their intended valid outcome and the
  selected Runner adapter controls runner creation through the registry;
- engine graph edge/epoch/invalidation closure, objective hard-constraint
  precedence, metric unit/normalization and stale/mixed estimate rejection;
- joint model-variant/partition/Provider feasibility, exact-constraint,
  candidate-budget and model/tokenizer/artifact lineage tests;
- deployment idempotency, readiness, cooldown/residency, drain and active-lease
  lifecycle validation;
- definition/revision canonicalization, external artifact/secret exclusion,
  dry-run zero-side-effect and apply/status/wait operation-handle tests;
- RuntimeJournal partial-write/corruption/version/lock/quota/compaction/
  retention/non-persistent-root, owner-permission, identity namespace/traversal/
  symlink/cross-tenant tests, protected request-envelope tamper/expiry/cleanup
  tests, plus restart at every lifecycle/request boundary;
- revision-bound Provider readiness, wrong/stale revision rejection, upgrade/
  rollback, durable request reopen/cancel/stream and graceful drain/delete tests;
- Python/CLI status and reason/evidence identity parity;
- revision-derived Slurm process-map cardinality, same-SIF project commands,
  one-NFD-per-node shared run binds, per-Provider GPU UUIDs, persistent state
  bind and allocation/app/request state separation;
- both admission/scheduling scopes, adapter-declared mixed/homogeneous batching,
  prefill/decode arbitration, fairness, preemption and authorized hedging tests;
- distributed execution fault injection before/after prepare, revalidation,
  commit, certificate publication, activation, result publication and release,
  including dropped/duplicated/reordered operations and partial receipt sets;
- requester crash before/after certificate construction, same-identity resume,
  stale attempt fencing and exactly one visible terminal result;
- competing deployment writers, stale lifecycle epochs, provider boot-epoch
  changes, network partitions and destructive-action fail-closed behavior;
- periodic idle-time orphan cleanup with bounded prepared/committed/executing
  lease, reservation, session, cache-pin and runner-handle retention;
- streaming output-commit/checkpoint recovery without duplicate visible output;
- observer idempotency/state replay/failure isolation and least-input privacy;
- typed tuning parameter boundary/wrong-type/wrong-scope/undeclared-key tests;
- cache action-kind/epoch/atomic-state tests;
- named defaults reproduce characterized current decisions and partial suites
  evidence every omitted default;
- standalone external wheel registration, allowlist, version/digest and missing
  dependency contract tests;
- partition/assignment/scheduling/tuning/cache/admission/recovery/target
  decision validation plus adapter-registry validation;
- same-input/configuration/seed replay and two-suite concurrency isolation;
- out-of-tree native runner compile/link and execution smoke;
- Core rejection matrix for stale/infeasible/unauthorized/invalid-lease results;
- 100 paired concurrent assignment repetitions;
- native plan/provider/dependency/cache/Qwen generation-session tests;
- security and negative token/permission/replay regressions;
- APP façade and deployment compatibility tests;
- package-build tests, static container-manifest/template/handoff tests and
  Core-only import smoke; no Docker/Podman/Buildah/Apptainer execution.

### Operational workflow gate

From clean installed owner profiles with external artifacts and a persistent
state root, run one MiniNDN validate -> resolve -> dry-run -> apply -> ready/
active -> submit -> certified result -> requester restart/open -> drain ->
inactive workflow. Preserve command/config/revision/candidate/request/result/
journal identities and every negative outcome. This is a correctness gate, not
a remote deployment or performance claim.

### Network/performance gate

Run ten frozen baseline/treatment pairs with one 60-second measured run per cell,
no automatic rerun, and identical candidate inputs, topology, workload, warmup,
logging, timeouts and sampler settings. Predeclare pair order and seeds before
execution. Compare completion, failure, p50/p95, throughput and resource/queue
evidence; report every cell, median paired relative change and a 95% paired
bootstrap interval. A correctness/completion regression, or a latency/throughput
interval crossing the 5% non-regression margin, blocks compatibility deletion.
Neutral, negative and failed cells remain measured outcomes.

### iTiger boundary

Spec 111 does not authorize a Slurm job. If a later Spec 110 continuation or new
spec validates the separated implementation, it must create a new candidate and
pass the applicable source/runtime/container/SIF/model/network/evidence offline
gates before explicit submission authorization. A pre-Spec-111 image is substrate
evidence only; the concrete post-separation work is appended to Spec 110 through
`specs/110-itiger-qwen-live-inference/handoffs/spec111-separation.md`.

All Spec 111 distributed network, security, failure, operational and performance
acceptance runs use MiniNDN. Local unit/native/package/static checks remain local.
The first OCI image build, SIF generation, Apptainer invocation and iTiger job
are intentionally deferred together to Spec 110 so container cost is paid only
after the Core/APP implementation and MiniNDN gates are mature.

## Project Structure

### Documentation (this feature)

```text
specs/111-ndnsf-di-core-app-separation/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── traceability.md
├── contracts/
│   ├── ownership-matrix.md
│   ├── distributed-inference-engine.md
│   ├── distributed-execution-consistency.md
│   ├── deployment-and-invocation-workflow.md
│   ├── itiger-slurm-apptainer-handoff.md
│   ├── execution-intent-and-feedback.md
│   ├── model-variant-policy.md
│   ├── provider-assignment-policy.md
│   ├── optimization-extension.md
│   ├── assignment-context.md
│   ├── compatibility-manifest.md
│   └── installation-profiles.md
├── checklists/
│   └── requirements.md
└── tasks.md
```

### Source Code (repository root)

```text
NDNSF-DistributedInference/
├── ndnsf_distributed_inference/
│   ├── core/                 # new deep execution modules
│   ├── app_sdk/              # migrated APP façade
│   ├── planner/              # application-owned policy implementation
│   ├── sdk/                  # public external optimization interfaces/loader/test kit
│   ├── adapters/             # optional model-family adapters
│   ├── ops/                  # CLI/operations adapters
│   ├── compatibility/        # bounded re-exports/translators
│   ├── runtime_v1.py         # temporary compatibility adapter
│   ├── app.py / gui.py       # temporary compatibility adapters
│   └── __init__.py           # bounded aggregate interface
├── cpp/ndnsf-di/             # retained native execution reference
└── setup.py / packaging metadata

tests/python/
tests/ndnsf-di/
tests/container/
examples/python/NDNSF-DistributedInference/
Experiments/
packaging/ndnsf-di-container/
```

**Structure Decision**: Use an owner-oriented internal module extraction first,
with bounded compatibility adapters, because it preserves current callers and
native behavior while making dependency direction mechanically enforceable.
Physical distribution splitting follows the isolation gate rather than preceding
it.

## Post-Design Constitution Re-check

All five constitution principles remain PASS. The design adds no new top-level
protocol name, authority, planner service, cluster persistence engine, public
state-store SPI, consensus layer or security bypass. The fixed local
`RuntimeJournal` is required only to survive APP process restart and is bounded
by existing requester/deployment authority. It extends the existing authenticated lease/Selection/result
contract only where fencing and a complete commit certificate require it. Validation is
MiniNDN-first and candidate-bound. English/Chinese documentation synchronization
and the completion bell are explicit tasks.

## Complexity Tracking

| Added mechanism | Why needed | Simpler alternative rejected because |
| --- | --- | --- |
| Typed optimization SDK | Lets an independent algorithm team replace objectives without editing framework code | A single generic callback would expose too much authority and be harder to validate |
| Process-local `DistributedInferenceEngine` and decision DAG | Gives the ten policies a real composition root, explicit epochs, invalidation/re-entry and evidence without a network coordinator | Leaving orchestration implicit would make the public ports callable but not a coherent engine |
| Ten narrow Python policies plus Runner adapter and optional outcome observer SPIs | Keeps typed optimization ownership while separating runner creation and one-way learning feedback | Turning runner/observer into policies grants the wrong authority; adding estimator/state-store/transaction policies duplicates suite or mechanism ownership |
| Shared metric-aware objective, estimate envelope and engine snapshot | Makes independently developed algorithms interpret units, uncertainty, constraints and telemetry consistently | Per-policy dictionaries or unnormalized weighted sums cannot enforce comparable objectives, freshness or lineage |
| Distributed execution/deployment consistency certificate and fencing | Prevents partial multi-Provider commit from becoming executable, fences crashed/stale requesters and lifecycle writers, and bounds orphan resources | Best-effort abort alone cannot prove complete authority after message loss; global consensus would be disproportionate and contradict the requester-coordinated architecture |
| Immutable deployment revision, fixed RuntimeJournal and durable APP operation/request handles | Turns separated libraries into a restart-safe validate/apply/use/drain workflow without adding cluster coordination | Current APPDeployment is read-only, `deploy_plan()` is metadata-only and process-local Future/state cannot satisfy crash recovery or rollback |
| Immutable iTiger runtime-allocation handoff | Lets the existing Slurm/Apptainer adapter launch infrastructure while APP owns revision/model lifecycle and preserves candidate identity | Treating Docker or Slurm RUNNING as deployment would duplicate authority, lose state/model bind guarantees and relabel a pre-separation image |
| Allowlisted opt-in entry points | Enables standalone wheels while keeping Core import deterministic | Automatic global discovery executes arbitrary installed code and leaks state between APP instances |
| AssignmentContext | Removes concrete process-global concurrency leak | Locking the environment variable still uses ambient state and cannot express lineage safely |
| Compatibility manifest/adapters | Enables migration-before-deletion across a large public surface | Immediate import/package break would invalidate callers and obscure behavior regressions |
| Owner-specific install profiles | Proves Core purity before risky physical package split | File placement alone does not prove optional implementations are absent at runtime |
