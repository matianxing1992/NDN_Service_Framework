# Contract: DistributedInferenceEngine Composition

## Purpose and ownership

`DistributedInferenceEngine` is the APP-owned, process-local composition root
that makes the optimization ports a coherent inference engine. It owns one
immutable `OptimizationSuite`, `ExecutionAdapterRegistry`,
`OptimizationObjective`, `EngineDecisionGraph`, decision-executor set, optional
`OptimizationObserver` and evidence/outcome sinks. It delegates eligibility,
authority, lease, readiness, state,
deadline and result validation to Core/provider mechanisms.

It is not an NDN service, cluster-global scheduler, new network coordinator,
security authority, persistence engine or process-global singleton. Its
`APPClient` owner coordinates only the current request attempt over the existing
authenticated lease and Selection services; the Engine itself gains no network
identity or Provider authority. Deployment, client
and provider processes may instantiate role-specific engine configurations that
share the same graph/contract identity without sharing mutable process state.

## Required public operations

The concrete Python API may use constructors/factories appropriate to the APP
surface, but it MUST expose these semantic operations:

| Operation | Input | Output |
| --- | --- | --- |
| `configure` | immutable suite, adapter registry, decision graph, executor identities, objective defaults and evidence sink | engine identity/digest or typed configuration rejection |
| `plan_session` | immutable deployment revision, model alternatives/exact constraint, deployment facts, workload forecast and objective | bounded candidates plus optional idempotent lifecycle action and resulting revision/snapshot lineage |
| `plan_request` | active deployment revision, prepared-session lineage, fresh snapshot and request objective | one validated revision/model/plan/Provider/target candidate plus cache affinity |
| `prepare_execution_intent` | selected decisions, policy-state lineage, requester/request/attempt authority and reservation preconditions | certified `ValidatedExecutionIntent` with complete authenticated Provider receipts, or non-executable abort/re-decision evidence |
| `dispatch_request_work` | admitted dependency-ready DAG work and assignment-authorized replicas | validated request-DAG dispatch/hedge decision |
| `dispatch_provider_work` | provider-admitted ready work, provider snapshot and adapter hard bounds | validated provider-local batch/scheduling/tuning decision plus selected adapter execution handle |
| `complete_or_recover` | result/cache/progress/checkpoint/failure facts, commit certificate, durable rendezvous evidence and remaining budget | terminal evidence/outcome or one validated bounded same-identity recovery/re-entry directive |

No operation exposes private keys, tokens, authority-changing handles or raw
mutable registries to policy code.

`APPDeployment` and `APPClient` expose the operator/request operations defined
in [`deployment-and-invocation-workflow.md`](deployment-and-invocation-workflow.md).
Those façades call the Engine; they are not additional policy graph nodes.
`InferenceRequestHandle` and `RuntimeJournal` preserve invocation/recovery
identity without entering policy input.

## Decision graph

Normal dependencies are:

```text
control/session epoch:
  objective + workload forecast + snapshot
    -> deployment lifecycle proposal/application
    -> new lineage-bound snapshot for later requests

request epoch:
 active deployment revision + objective + snapshot
  -> engine-request admission
  -> model-variant candidates (only when alternatives exist)
  -> variant-bound partition candidates
  -> execution-target candidates per plan-role-Provider alternative
  -> pre-assignment cache decision/affinity
  -> selected model variant + plan + Provider + role targets
  -> prepare-all/revalidate/commit-all/certify execution intent
  -> request-DAG scheduling
  -> provider-local admission
  -> compatible provider-local batch/order/capacity
  -> typed execution tuning
  -> selected RunnerAdapter -> Core/native execution
  -> progress/output commit/checkpoint
  -> post-execution cache action or bounded recovery
  -> asynchronous OptimizationOutcome observation
```

Deployment lifecycle is not an unconditional per-request predecessor. A
no-feasible request may use one declared bounded re-entry edge to request a
control-plane action, obtain a new snapshot and repeat planning. The graph is
not required to execute every node for every request. Each node
declares applicability, owner, input/output types, validator, invalidation
triggers and evidence. Only declared recovery edges may re-enter an earlier
node, and they remain bounded by the original deadline/attempt budget.

## Objective contract

`OptimizationObjective` separates:

- hard constraints: exact model/operator choices, deadline, TTFT/TPOT maxima,
  throughput/quality minima, cost ceiling, availability and safety bounds;
- weighted preferences: latency, TTFT, TPOT, throughput, quality, cost, energy,
  fairness/priority and recovery preference;
- deterministic tie-break rules and objective identity/digest.

Every metric additionally declares unit, direction, aggregation/quantile,
normalization and missing-data semantics. Raw weighted values are never combined
without compatible normalization. Hard feasibility is evaluated before ranking.

Hard constraints always dominate weights. A model-specific quality metric has a
stable identity/provenance but is interpreted by the APP/adapter; Core validates
structural membership and declared bounds, not unmeasured semantic quality.

## Snapshot contract

`EngineSnapshot` binds one epoch of:

- model/deployment/artifact residency, active sessions, leases and reservations;
- Provider capability, queue/worker state and GPU compute/memory;
- network RTT, bandwidth and loss with measurement lineage;
- exact/semantic/prefix/KV cache state and state epoch;
- runtime/adapter/device compatibility and availability;
- workload shape, timestamps, freshness bounds and estimate provenance.
- active progress, output-commit and checkpoint epochs.

Each policy receives a typed least-input projection. Per-source sample epochs
may differ when lineage/freshness tagged, but every merge creates a new snapshot
digest; untracked/internally inconsistent or stale facts invalidate dependent
proposals and cannot be upgraded by policy. Forecasts and observations
use `EstimateEnvelope` with source, confidence, horizon and freshness; prompt
text, raw tensors and authority-bearing material are excluded by default.

## Atomic execution-intent contract

The engine MUST bind selected model, plan, assignment, cache affinity, target,
tuning, objective/snapshot and policy-state lineage in one
`ValidatedExecutionIntent`. The initiating APPClient identity coordinates the
attempt, while owning Provider mechanisms prepare, revalidate and acquire
reservations using the existing lease service. Every selected Provider must
return an authenticated commit receipt before the coordinator can construct one
immutable `ExecutionCommitCertificate`. Selection/activation is rejected
without that certificate. Any changed precondition aborts the whole intent,
releases reachable resources and permits only bounded re-decision; expiry and
periodic cleanup bound unreachable orphans. A torn tuple is never executable or
evidentiary. The normative crash, partition, fencing, deployment lifecycle and
cleanup rules are in
[`distributed-execution-consistency.md`](distributed-execution-consistency.md).

Requester restart may resume only under the same requester identity and current
attempt epoch using authenticated receipts/rendezvous evidence. A higher attempt
epoch fences lower work. Cross-identity takeover, quorum and leader election are
outside this API.

## Scheduling, admission and adapter capability scopes

`AdmissionPolicy` has `ENGINE_REQUEST` and `PROVIDER_LOCAL` scopes.
`SchedulingPolicy` has `REQUEST_DAG` and `PROVIDER_LOCAL` scopes. Cross-role/
transfer dispatch belongs to APPClient; local continuous batching belongs to
APPProvider. Runner adapters declare `BatchCapability`; mixed prefill/decode
batching is allowed only when the selected adapter advertises it. Hedging may
use only assignment-authorized replicas.

## Outcome and state contract

Accepted/rejected intents and completed/failed attempts emit bounded immutable
`OptimizationOutcome` records. An optional independent
`OptimizationObserver.observe` receives them outside the current request's
critical path under a separate budget and is idempotent by outcome identity.
Observer failure cannot change the inference result. Stateful extensions record
the policy state epoch/digest consumed by every decision and own persistence/
concurrency; Core supplies no plugin database.

## Validation and evidence

For every applicable node, the engine records policy identity/version/digest,
objective and snapshot projection digests, input/configuration/seed, policy
state epoch/digest, budget,
output digest, validator result, duration, status and explicit fallback. A
not-applicable node records a reason such as
`NOT_APPLICABLE_EXACT_CONSTRAINT`; it is not silently omitted.

The engine applies no proposal until the owning Core/provider validator accepts
it. Timeout, malformed result, stale snapshot, wrong epoch, unavailable adapter
or undeclared graph edge fails closed or uses only a named fallback.

Metadata-only `PreparedPlanSession` reuse cannot bypass active-revision,
readiness, certificate or request-handle validation. Mixed deployment revisions
are rejected unless their explicit compatibility contract permits them.

## Implementation gate

Large-scale SDK implementation is blocked until this contract, the distributed
execution/deployment consistency contract, model-variant contract, typed
objective/snapshot entities, refined policy contracts, tasks and traceability
have strict structure PASS, deterministic analysis with no unresolved
CRITICAL/HIGH finding and a code-aware pre-implementation PASS. Engine
execution-intent implementation additionally waits for T064 to execute the
contract/MiniNDN crash, partition, fencing and orphan-cleanup gate successfully.
