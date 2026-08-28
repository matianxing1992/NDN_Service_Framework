# Data Model: NDNSF-DI Core/APP Separation

## CoreExecutionPlan

Immutable workload-neutral execution description.

Fields:

- `plan_id`, `plan_digest`, `schema_version`
- `model_identity` as opaque identity, not model-family behavior
- `roles[]`
- `dependencies[]`
- `artifact_requirements[]`
- `execution_invariants`
- `created_by` planner identity and source lineage

Rules:

- Core validates role/dependency consistency and immutable identity.
- Model-specific split meaning remains adapter-owned metadata.
- Existing readable plan schema remains supported during migration.

## PlanCandidateSet

Immutable output of one partition-planning decision.

Fields:

- planner identity, input/configuration/seed digests
- one or more `CoreExecutionPlan` candidates
- per-candidate objective explanation and feasibility metadata
- candidate-set digest and decision epoch

Rules:

- `PartitionPlanner` generates candidates but does not bind Providers.
- `ProviderAssignmentPolicy` selects one candidate identity together with its
  Provider assignments, permitting joint partition/assignment scoring.
- Core validates the selected plan and assignment independently.

## CandidateSnapshot

Immutable planning input for one request/attempt.

Fields:

- `snapshot_id`, `snapshot_digest`, `sampled_at_ms`, `max_age_ms`
- `request_id`, `template_id`, `attempt`
- `eligible_candidates_by_role`
- `rejected_candidates[]` with Core reason codes
- `telemetry_lineage`, `discovery_lineage`, `policy_input_metadata`

Rules:

- Core creates the normalized snapshot.
- Stale or malformed facts cannot be upgraded by policy.
- Policy sees only the Core-eligible set plus explanatory rejections.

## OptimizationObjective

Immutable cross-policy objective with hard constraints separated from weighted
preferences.

Fields:

- `objective_id`, `schema_version`, `objective_digest`
- ordered `ObjectiveMetricSpec` entries with stable metric identity, unit,
  minimize/maximize direction, aggregation/quantile, normalization, missing-
  data rule, hard bound or weight, and measurement source requirements
- hard deadline/TTFT/TPOT/throughput/quality/cost/availability constraints
- weighted latency, TTFT, TPOT, throughput, quality, cost, energy, fairness and
  recovery preferences only after declared normalization
- tenant/request priority and deterministic tie-break rules

Rules:

- Exact operator constraints and hard bounds always dominate weights.
- A model-specific quality metric is identified but interpreted by the
  authorized APP/model adapter, not by workload-neutral Core.
- Every policy decision records the same objective digest or a declared
  least-input projection digest.
- Feasibility under hard constraints is evaluated before preference ranking;
  incomparable raw metrics cannot be silently added.

## EstimateEnvelope

Typed observation/forecast/unknown wrapper for workload and resource facts.

Fields:

- fact identity and kind: `OBSERVED`, `PREDICTED`, or `UNKNOWN`
- scalar value or bounded distribution/quantiles and unit
- confidence/calibration identity, prediction horizon and source identity
- `sampled_at_ms`, `max_age_ms`, lineage digest and missing-data rule

Rules:

- Policies can distinguish measurement from prediction and unknown.
- A value outside its horizon/freshness is unknown, not a current observation.
- Output length, arrival rate, service time, memory, queue and network estimates
  use the same envelope semantics.

## EngineSnapshot

Immutable aggregate of actionable facts for one engine decision epoch.

Fields:

- `snapshot_id`, `snapshot_digest`, `epoch`, `sampled_at_ms`, `max_age_ms`
- model/deployment/artifact residency and active lease/session facts
- Provider capability, queue/worker, GPU memory/compute and reservation facts
- network RTT/bandwidth/loss matrix with measurement lineage
- exact/semantic/prefix/KV cache state and state epochs
- registered runtime/adapter/device compatibility and availability
- request/workload shape and historical estimate lineage
- Core-owned progress, output-commit and checkpoint epochs for active attempts

Rules:

- APP/Core mechanisms assemble the snapshot; policy cannot upgrade rejected or
  stale facts.
- Each hook receives only a typed least-input projection.
- Prompt text, raw tensors, credentials, tokens, decrypted policy material and
  unrelated tenant data are excluded unless an explicit contract requires and
  authorizes one application metadata field.
- Per-source fact epochs may differ when each is lineage/freshness tagged. Any
  merge produces a new snapshot/digest; untracked or dependency-inconsistent
  mixtures are rejected and dependent decisions are revalidated.

## ModelAlternativeSet

Operator/APP-authorized alternatives for optional model-variant optimization.

Fields:

- `alternative_set_id`, `digest`, `exact_constraint` flag
- alternatives with model family/identity/size, revision, tokenizer,
  precision, quantization, artifact digests, adapter identity and optional
  draft-model compatibility
- quality metric/floor declarations and provenance

Rules:

- One exact alternative makes model-variant optimization not applicable.
- A policy cannot synthesize an alternative or alter tokenizer/artifact identity.
- Quality metadata is evidence-bearing input, not a Core assertion of measured
  model quality.

## ModelVariantCandidateSet

Bounded ranked/pruned proposal from a `ModelAlternativeSet`.

Fields:

- retained alternative identities and complete model/tokenizer/artifact tuples
- optional compatible draft-model tuples
- policy/objective/snapshot/alternative-set digests
- per-candidate explanation, rejected/pruned alternatives, pruning budget and
  decision evidence identity

Rules:

- APP validates membership, quality-contract compatibility and exact constraints
  before deployment or partitioning.
- `PartitionPlanner` expands retained variants into variant-bound plans.
- `ProviderAssignmentPolicy` selects one retained variant and plan with the
  assignment; it cannot synthesize a variant.
- An exact constraint creates one singleton without invoking policy.

## EngineDecisionGraph

Machine-readable process-local orchestration contract.

Fields:

- `graph_id`, `schema_version`, `graph_digest`
- nodes with policy/adapter/mechanism kind, decision epoch, APP invocation owner,
  typed input/output and validator
- dependency edges, invalidation triggers, allowed recovery/re-entry edges
- evidence requirements and terminal success/failure states

Rules:

- Cycles are forbidden except bounded recovery edges declared by the graph.
- Every policy node maps to one `DecisionPort` and decision-point inventory row.
- The graph is not a network service, global scheduler, authority or persistence
  engine.

## DistributedInferenceEngineConfiguration

Immutable configuration of one APP-owned process-local engine instance.

Fields:

- engine identity/digest and owner role (`deployment`, `client`, `provider`)
- `OptimizationSuite`, `ExecutionAdapterRegistry`, decision graph and executor
  identities
- objective defaults, snapshot providers, explicit fallbacks and evidence sink
- optional `OptimizationObserver`, observer executor and outcome sink identity

Rules:

- No process-global default registry or singleton.
- APP owns extension processes/lifecycle; Core owns validation/execution only.
- Configuration stays immutable for one request/attempt lineage.
- Observer failure cannot change a current inference result.

## ProviderAssignmentPolicyDescriptor

Identifies an application-owned ordinary/multi-role Provider assignment
implementation.

Fields:

- `policy_name`, `policy_version`, `policy_digest`
- `objective_schema`, `configuration_digest`
- `deterministic` flag

Rules:

- Same name with different version/digest is a different policy identity.
- Core does not interpret objective-specific fields.

## OptimizationExtensionDescriptor

Identifies one externally supplied optimization implementation.

Fields:

- `name`, `semantic_version`, `contract_version`
- `distribution_name`, `source_digest`, `build_identity`
- `supported_hooks`, `model_families`, `runtime_backends`
- `deterministic`, `configuration_schema`, `configuration_digest`

Rules:

- Identity is the complete name/version/contract/digest tuple.
- Discovery never implies selection or execution.
- Installed identity must satisfy the APP/operator allowlist.

## DecisionPort

Core-owned structural contract for invoking one external decision without
depending on SDK implementation.

Fields/behavior:

- one immutable Core request type;
- one typed proposal/result type;
- one invocation method;
- associated Core validator and reason-code set.

Rules:

- SDK re-exports/adapts the canonical Core types; it does not clone them.
- Core may receive an injected port implementation but never imports SDK/APP.
- The port cannot expose tokens, private keys or authority-changing handles.

## DecisionBudget

Immutable invocation limit supplied by APP/Core mechanism.

Fields:

- `deadline_ms`, `work_unit_limit`, optional CPU/memory resource limits
- maximum model variants, plans per variant, Provider candidates and total
  expanded solution candidates
- `cancellation_id`
- `random_seed`
- `request_id`, `attempt`, `candidate_identity`

Rules:

- Extension cannot extend the original request deadline or recovery budget.
- Timeout/cancellation produces typed evidence and no partial decision.
- Pruning/truncation is deterministic under `random_seed`, preserves hard
  constraints and records all pruned identities.

## OptimizationSuite

Instance-scoped selection of extension hooks.

Fields:

- suite descriptor/digest
- optional `deployment`, `model_variant`, `partition`, `provider_assignment`, `scheduling`,
  `execution_tuning`, `cache`, `admission`, `recovery`, and
  `execution_target` policy identities
- explicit per-hook fallback identities

Rules:

- At most one primary and one explicit fallback per hook.
- Missing hooks select named members of `DefaultOptimizationSuite` through the
  same executor/evidence path; hidden legacy/default branches are forbidden.
- Suite configuration is immutable for one request/attempt lineage.
- APPDeployment owns deployment/deployment-target execution; APPClient owns
  engine admission, model-variant/partition/Provider-assignment/request-target
  and request-DAG scheduling; APPProvider owns provider admission,
  provider-local scheduling/tuning/cache/provider-target execution;
  recovery reuses the failed-path executor; Core owns neither registry nor
  plugin process.

## ExecutionAdapterRegistry

Instance-scoped registry of Runner adapter factories, independent of
`OptimizationSuite`.

Fields:

- registry identity/digest
- adapter identity -> `RunnerAdapter` registration
- optional explicit adapter fallback identities

Rules:

- Registration never selects or executes an adapter.
- Only an adapter identity selected from `ExecutionTargetCandidateSet` by a
  validated `ProviderAssignmentDecision` may be resolved and created.
- Duplicate identity/version/digest conflicts fail before inference.
- The registry has no process-global default or import-time discovery.

## DecisionPointInventory

Closed classification of every production policy-bearing decision site.

Fields:

- `decision_id`, source symbols and callers
- Python port and APP invocation owner
- Core/provider validator and reason-code set
- versioned default implementation identity
- compatibility target and contract/evidence paths
- classification: `policy-bearing` or explicit `mechanism-only` invariant

Rules:

- Every production choose/score/rank/order/plan/bounded-tuning site is present.
- New or moved unclassified sites fail static validation.
- A mechanism-only classification must name the invariant that makes external
  replacement unsafe or meaningless.

## DefaultOptimizationSuite

Versioned suite containing migrated current policy behavior plus the explicit
exact-or-compatible-set reference default for all ten policy ports.

Rules:

- Implements the same public Python protocols and proposal schemas as external
  wheels.
- Default implementations are owned by Planner/model-adapter distributions;
  SDK owns contracts only and APP composes explicitly installed defaults.
- Has no privileged Core/native bypass.
- Each decision records its default identity and configuration digest.
- Frozen parity fixtures compare pre-migration and default-suite decisions.

## DefaultExecutionAdapterRegistry

Versioned registrations containing migrated current Python/native runner
adapters.

Rules:

- Uses the same `RunnerAdapter` SPI as external registrations.
- Has no privileged native/Core factory bypass.
- Creation records adapter identity, selected execution target and artifact
  lineage.

## OptimizationDecisionEvidence

Reproducibility and failure record for one hook invocation.

Fields:

- extension/suite/contract identity
- input, configuration, seed and output digests
- `policy_state_epoch` and `policy_state_digest` consumed by the decision
- start/end/duration and decision budget
- status and typed reason codes
- fallback identity and trigger, when used

Rules:

- Required for accepted and rejected decisions.
- Same input/configuration/seed must reproduce the output digest before
  deployment/research promotion.
- Evidence never contains tokens, private keys or decrypted policy material.
- Concurrent stateful policies declare snapshot/version semantics; suite
  instances never silently share mutable state.

## ProviderAssignmentDecision

Ordinary one-role or multi-role policy proposal returned to Core.

Fields:

- `decision_id`, `policy_descriptor`, `snapshot_id`, `snapshot_digest`
- `selected_model_variant_id`, `selected_plan_id`, `role_assignments`,
  `role_execution_target_ids`, `requested_multiplicity`
- `score_breakdown`
- `ranked_alternatives`
- `policy_rejections`
- `decided_at_ms`

Rules:

- Must reference the exact current snapshot.
- Every role is assigned exactly as allowed by plan/request multiplicity.
- Core revalidates selected candidates and leases before acceptance.

## AssignmentContext

Immutable accepted execution binding for one request attempt.

Fields:

- `request_id`, `template_id`, `plan_digest`
- `decision_id`, `policy_descriptor`, `snapshot_digest`
- `role_assignments`
- `lease_bindings`
- `deadline_ms`, `attempt`, `attempt_epoch`
- `candidate_identity`, `evidence_epoch`

Rules:

- Request-scoped; never sourced from mutable global environment.
- All selected roles and lease bindings are Core-validated.
- Replan creates a new decision/context and increments bounded attempt lineage.
- Stale attempt results cannot become authoritative.

## ModelAdapterDescriptor

Optional planner/runner implementation identity.

Fields:

- `adapter_name`, `adapter_version`, `adapter_digest`
- `model_families`, `model_formats`, `runtime_backends`
- `planner_capability`, `runner_capability`
- `optional_dependencies`

Rules:

- Registration is explicit.
- Missing optional dependencies fail only adapter activation.
- Adapter output conforms to CoreExecutionPlan/runner contracts.

## ExecutionTargetCandidateSet

Bounded policy proposal of compatible runner targets per plan-role-Provider
alternative; final target selection remains part of assignment.

Fields:

- candidate identity, plan/role/Provider binding, adapter/runtime/engine identity
  and version
- device kind/id and bounded runtime parameters per candidate
- per-role `BatchCapability`, KV/prefix/checkpoint and device/compute-capability
  compatibility digest advertised by the selected adapter
- explicit compatible fallback target identities, when configured
- input/configuration/output digests and explanation

Rules:

- Planner registry identity is not part of this decision.
- Every proposed adapter must exist in the applicable APP/provider instance's
  `ExecutionAdapterRegistry`.
- Registration/availability/artifact compatibility is revalidated before
  assignment and again before adapter creation.
- `ProviderAssignmentPolicy` selects only supplied target candidates together
  with variant/plan/Provider bindings; it cannot synthesize a target.

## DeploymentLifecycleDecision

Typed deployment/session-epoch proposal.

Fields:

- action: `USE_EXISTING`, `ACTIVATE`, `RESERVE`, `PREWARM`, `SCALE_OUT`,
  `SCALE_IN`, or `UNLOAD_OR_EVICT`
- deployment/model/artifact identity, replica delta/target, reservation and
  prewarm/unload parameters
- action idempotency key, expected lifecycle state/epoch, minimum residency,
  cooldown/hysteresis and bounded drain deadline
- active lease/session facts, objective/snapshot/policy lineage and explanation

Rules:

- Never invoked per generated token or unconditionally per request.
- Scale-in/unload cannot invalidate active leases or sessions.
- Mechanism validates capacity, artifact identity and exact constraints.
- Repeated action identity is idempotent; abort/rollback releases reservations.

## SchedulingDecision

Atomic dispatch proposal with an explicit scope.

Fields:

- scope: `REQUEST_DAG` or `PROVIDER_LOCAL`
- stable compatible work/batch membership and dispatch order
- worker concurrency and queue-window grants
- adapter-declared compatibility key, phase, fairness/priority,
  preemption/resume and optional assignment-authorized hedge directives
- readiness/capacity/objective/snapshot/policy lineage

Rules:

- Batch members satisfy the adapter's `BatchCapability`; mixed prefill/decode is
  allowed only when explicitly advertised.
- Proactive hedging belongs here; post-failure transitions belong to recovery.
- Scheduling cannot choose model, partition, Provider or runtime.
- Request-DAG scheduling cannot mutate provider-local queues, and provider-local
  scheduling cannot create cross-role/Provider assignments.

## TuningParameterSpec And ExecutionTuningDecision

Closed typed bounds and selected per-invocation values.

Fields:

- parameter name, scalar/enum type, scope, minimum/maximum or allowed values
- compatibility predicates and provider hard limit
- selected values for segment/microbatch, compression, transfer chunk,
  prefetch depth, overlap or speculative-decoding window when declared
- objective/snapshot/policy lineage

Rules:

- Undeclared keys, wrong types/scopes and out-of-range values fail closed.
- Tuning cannot dispatch work or change worker/queue capacity.

## CacheDecision

Typed cache action for a declared phase.

Fields:

- decision kind: `LOOKUP_OR_REUSE`, `PLACE`, `PREFETCH`, `ADMIT`, `RETAIN`,
  `EVICT`, or `MIGRATE_OR_REPLICATE`
- phase: `PRE_ASSIGNMENT` or `POST_EXECUTION`
- object/key/security/state epoch, placement/affinity and atomic update intent
- objective/snapshot/policy lineage

Rules:

- Pre-assignment decisions may emit affinity but never final compute assignment.
- Post-execution mutation requires current state epoch and atomic application.
- Cache-object admission is distinct from inference invocation admission.

## AdmissionDecision

Scoped accept/defer/reject proposal.

Fields:

- scope: `ENGINE_REQUEST` or `PROVIDER_LOCAL`
- action, reason, optional defer-until/deadline and applicable quota identity
- mandatory-floor, objective/snapshot/policy and request/attempt lineage

Rules:

- Engine admission runs before expensive planning when configured; provider
  admission runs only after assignment against local capacity/readiness.
- Either scope may be stricter than mechanism floors; neither can override a
  rejection or weaken a mandatory floor.

## ProgressCheckpoint

Core-owned streaming/generation recovery fact.

Fields:

- request/attempt/output epoch, committed output offset/token sequence
- checkpoint identity/digest, compatible model/plan/target and state epoch
- restart/resume capability and visibility/acknowledgement state

Rules:

- Only mechanism-advertised boundaries are eligible recovery inputs.
- Duplicate, reordered or stale visible output epochs fail closed.

## ValidatedExecutionIntent

Atomic binding prepared before side-effecting execution.

Fields:

- intent/idempotency identity and state: `PREPARED`, `COMMITTED`, `ABORTED`
- objective/snapshot and policy-state epoch/digest
- model variant, plan, Provider assignment, target, tuning and cache-affinity
  decision identities
- reservation/lease preconditions and acquired-resource release list
- prepare/revalidate/commit/abort evidence

Rules:

- Commit revalidates every bound identity/epoch together.
- Failure at any boundary aborts the whole intent and releases reachable
  resources; lease deadlines bound resources whose Providers are unreachable.
- The intent is not executable until one complete `ExecutionCommitCertificate`
  exists. Partial/torn execution state is never externally authoritative.

## RequestCoordinatorBinding

Per-attempt authority record owned by the initiating requester identity.

Fields:

- requester identity, request ID and positive monotonic attempt epoch
- intent/objective/snapshot/plan digests and original deadline
- authorized Provider/role set and result rendezvous identity
- state, last durable evidence identity and optional explicit delegation proof

Rules:

- The initiating `APPClient` identity coordinates prepare/commit/abort for this
  tuple; another identity cannot infer takeover authority from timeout alone.
- Same-identity restart may resume from authenticated receipts and durable
  evidence. A higher attempt epoch fences every lower attempt.

## AuthenticatedProviderReceipt

Provider-authenticated evidence of one idempotent prepare, commit, abort, release
or activation operation over the existing lease service.

Fields:

- operation kind, version, requester/request/attempt and intent identities
- Provider identity, Provider boot epoch, lease identity/state/expiry/deadline
- plan/resource-binding/conflict-key digests and operation idempotency key
- response Data name, signer/certificate identity and canonical wire digest or
  equivalent verifiable proof
- status, typed reason and Provider-local evidence timestamp

Rules:

- Receipt identity covers every field used for activation or cleanup.
- Duplicate operations return the same logical receipt; conflicting reuse fails
  closed. A boot-epoch change invalidates old Provider authority.

## ExecutionCommitCertificate

Immutable evidence that every selected Provider committed the exact same
request attempt and execution intent.

Fields:

- certificate version, requester/request/attempt and intent identity
- objective/snapshot/model/plan/assignment/policy-state digests
- exact role/Provider/boot-epoch/lease/resource-binding tuple set
- complete canonical commit-receipt set and certificate digest
- execution deadline, result rendezvous and output-visibility epoch

Rules:

- The expected tuple set equals the receipt set exactly: no missing, duplicate
  or additional Provider is accepted.
- Selection/activation references this certificate. Providers validate their
  membership, current boot epoch, committed unexpired lease and all shared
  digests before transitioning to execution.
- The certificate provides atomic execution visibility, not simultaneous global
  state transition or availability during a partition.

## DeploymentLifecycleRecord And ActionCertificate

Fenced single-writer state for each deployed model/plan identity.

Fields:

- deployment ID, authorized owner identity and positive lifecycle epoch
- current state/state digest, desired action and action/idempotency digest
- expected previous state/epoch, Provider set and boot epochs
- complete prepare receipts, committed action certificate and terminal evidence

Rules:

- Only the configured owner may advance the lifecycle stream. Providers apply
  actions by compare-and-set on owner, lifecycle epoch and expected state digest.
- Exact replay is idempotent; stale/conflicting writers and destructive actions
  without a complete certificate fail closed.
- Spec 111 adds no leader election or automatic cross-identity takeover.

## DeploymentDefinition

Mutable operator intent loaded through the existing
`DistributedInferenceDeployment` configuration façade.

Fields:

- application/deployment/services and controller/trust/identity references
- exact model or authorized alternatives and request semantic constraints
- role/dependency/key-scope plan and policy/suite/adapter/runtime references
- resource/SLO/readiness/lifecycle constraints
- external `ArtifactReference` entries and security/staging/retention policy
- persistent state root, evidence/event destinations and rollback metadata

Rules:

- Definitions contain references to secrets/artifacts, never secret values or
  model bytes.
- Validation and dry-run create no Provider/lifecycle side effect.
- `DistributedInferenceDeployment` is a compatibility representation of this
  object, not a second source of truth.

## DeploymentRevision

Immutable resolved deployment identity.

Fields:

- deployment/revision ID, schema/contract version and canonical digest
- exact definition/source/policy/suite/adapter/runtime/profile digests
- exact model/tokenizer/artifact reference and security/configuration digests
- required capabilities/readiness minimums and compatibility matrix
- creator/time and optional previous rollback revision

Rules:

- All mutable paths/defaults resolve before digest creation.
- Any resolved difference creates a new revision; revision bytes are immutable.
- Provider readiness, lifecycle actions, plan sessions and requests bind exactly
  one revision.

## ArtifactReference

External model/tokenizer/runtime artifact contract.

Fields:

- logical identity/revision, URI or mounted-path reference
- digest, size, format, shard/range and executable/signature metadata
- access/credential reference without credential value
- staging/cache class, quota, retention and deletion owner

Rules:

- Provider readiness requires identity/digest/runtime compatibility.
- Deployment deletion releases only deployment-owned pins/staging data; shared
  external data remains unless an explicit owner action deletes it.

## DeploymentInstanceRecord

Desired/observed state of one revision across its Provider set.

Fields:

- deployment/revision and current lifecycle/action certificate
- immutable definition/revision state (`DRAFT/VALIDATED/RESOLVED`) and observed
  instance phase (`ABSENT/APPLYING/STAGING/WARMING/READY/ACTIVE/DRAINING/
  INACTIVE/DELETED`)
- reason-coded `Reconciling/Ready/Active/Degraded/Failed/Unknown` conditions
- required/observed role counts and Provider boot/revision epochs
- artifact/adapter/runtime/permission/capacity/readiness conditions
- reason, retryability, observed generation, event cursor and freshness

Rules:

- Liveness alone cannot satisfy READY.
- ACTIVE requires READY plus explicit admission activation.
- DEGRADED/FAILED/UNKNOWN are conditions, not alternative lifecycle phases;
  stale or partitioned observations never imply INACTIVE.
- Wrong/stale revisions are ineligible unless explicit compatibility is tested.

## RuntimeJournal

Fixed APP-owned restart/rollback evidence mechanism.

Fields:

- schema version, state-root identity and monotonic record/event sequence
- deployment revisions/lifecycle actions/rollback pointers
- requester bindings, protected request-envelope references, receipts, execution
  certificates and rendezvous references
- cleanup/fencing tombstones, checksums and retention/compaction metadata

Rules:

- Default storage is append-only local filesystem state under an operator-mounted
  persistent root with locking and atomic durable transitions.
- The root partitions records/spools by canonical application/NDN owner identity
  and deployment/request stream; cross-identity, traversal and symlink access
  fail before reads.
- No private keys, tokens, credentials, model bytes, raw prompts or tensors.
- Owner-only protected request wire envelopes live in an adjacent spool or
  durable NDN repository; the journal stores only name/digest/security/expiry.
- Corruption, lock/version/quota or unsafe persistence failures block new
  authority. This is not a policy, public state-store SPI or cluster database.

## DeploymentOperationHandle

Reopenable control-plane operation identity.

Fields:

- deployment/revision/action/lifecycle epoch and idempotency digest
- current state, terminal condition and event/journal cursor
- deadline, responsible owner and typed reason/retryability

Rules:

- Every mutating APPDeployment operation returns a handle.
- Restart reconciles its journal record with authenticated Provider evidence;
  local return values alone never prove success.

## PreparedPlanSession

Reusable client static metadata/reference handle.

Fields:

- deployment revision, plan fingerprint and published reference identities
- freshness/expiry and compatibility identity

Rules:

- It does not prove READY/ACTIVE, Provider model residency or lifecycle apply.
- Legacy `DeploymentSession`/`deploy_plan()` delegate to this metadata-only type/
  operation.

## InferenceRequestHandle

Durable request control and recovery identity.

Fields:

- requester/request/attempt and deployment revision
- state: `CREATED/PLANNING/PREPARING/CERTIFIED/EXECUTING/COMPLETED/FAILED/
  CANCELLED/EXPIRED`
- deadline, protected request-envelope reference, intent/certificate identity,
  result rendezvous and event cursor
- cancellation identity/reason and terminal evidence

Rules:

- Submit, synchronous/Future adapters and reopen share this identity.
- Submit returns only after the protected envelope and journal reference are
  durable; recovery never synthesizes missing/expired/tampered input.
- Status is monotonic per attempt and recovery accepts authenticated evidence.
- Cancellation is idempotent/attempt-fenced and never revokes an accepted result.

## RequestEnvelopeReference

Durable recovery reference to the existing protected request wire envelope.

Fields:

- requester/request identity and existing RequestMessage Data name or opaque
  owner-local locator
- protected wire digest, security context/attribute identity and expiry
- retention owner, terminal cleanup state and optional external repository ID

Rules:

- The referenced bytes are authenticated and confidentiality-protected before
  persistence; plaintext prompt/tensor data never enters journal/status/metrics.
- Reopen may republish/replan only after same-requester authorization and exact
  wire-digest verification.
- Retention dominates deadline/retry/result-rendezvous windows; terminal cleanup
  is idempotent and preserves only the fencing/digest tombstone.

## RuntimeAllocationHandoff

Immutable operations bridge from a completed Spec 111 candidate/revision to a
concrete OCI/SIF/Slurm allocation render.

Fields:

- candidate/source/offline-gate and deployment-revision digests
- OCI reference/digest, SIF SHA-256 and release-manifest digest
- model/tokenizer/artifact reference digests
- Slurm cluster/profile/GRES and allocation-process-map digest
- selected transport-probe digest when multi-node
- identity-set, persistent-state-root and evidence-destination identities
- render/submission identity and explicit-authorization state

Rules:

- Any changed field creates a new handoff; a pre-Spec-111 release is substrate
  evidence only.
- Render is side-effect free. Submission remains exactly-once and separately
  authorized under the concrete runtime spec.
- The handoff contains references/digests, never credentials, weights or mutable
  environment defaults.

## InfrastructureAllocationHandle

Operations-owned Slurm allocation identity, separate from APP deployment and
request authority.

Fields:

- runtime adapter and handoff digest
- scheduler job/allocation ID and requested/observed resources
- scheduler state, reason, event/log cursor and terminal evidence
- linked deployment operation/request IDs only after those states exist

Rules:

- PENDING/RUNNING/COMPLETED never imply READY/ACTIVE/request success.
- Cancellation of an allocation requests bounded APP drain when possible but
  does not forge APP terminal state after forced termination.
- A replacement allocation gets a new infrastructure handle and may reconcile
  only under the same authorized candidate/state-root contract.

## OrphanCleanupRecord

Provider-local proof of bounded reclamation independent of new request traffic.

Fields:

- Provider identity/boot epoch, sweep identity and monotonic sweep time
- expired lease, reservation, session, cache-pin and runner-handle identities
- previous state, terminal cleanup state, reason and reclamation timestamp
- retained attempt/lifecycle high-watermark tombstone identity and replay-window
  expiry, distinct from active resource ownership
- retry/failure evidence and next bounded retry deadline

Rules:

- Cleanup runs periodically and on operation entry; idle Providers cannot retain
  expired resources indefinitely.
- Cleanup is idempotent and cannot revive authority, publish output or cross a
  current boot/attempt/lifecycle epoch.
- Resource reclamation precedes fencing-tombstone garbage collection. The
  tombstone remains until no delayed operation/result can pass the declared
  replay window.

## ResultRendezvousRecord

Attempt- and certificate-scoped result visibility record.

Fields:

- requester/request/attempt, certificate and output epoch
- provider/role result identities, committed offsets and terminal digest
- visible terminal state, acknowledgement state and expiry

Rules:

- At most one attempt becomes externally visible. Duplicate/reordered/stale
  partial outputs cannot cross the certificate/attempt boundary.
- Same-identity requester recovery may fetch the terminal record; another
  requester identity needs explicit delegation.

## OptimizationOutcome And OptimizationObserver

Bounded asynchronous feedback independent of optimization policy.

Fields:

- unique outcome identity and decision/intent/execution/candidate lineage
- committed/rejected/failed status and standardized measured metric envelopes
- completion/failure/progress summary with sensitive payloads excluded
- `policy_state_epoch`/`policy_state_digest` before and after observer-owned
  updates, when the extension is stateful
- delivery attempt and observer evidence identity

Rules:

- `OptimizationObserver.observe` is idempotent by outcome identity and executes
  outside the current inference critical path under its own budget.
- Observer failure cannot change an inference result or Core availability.
- Persistence, checkpointing and concurrency control belong to APP/extension;
  Core supplies no plugin state database.

## CompatibilityEntry

One bounded legacy-to-owner mapping.

Fields:

- `legacy_surface`, `surface_kind`
- `canonical_owner`, `canonical_target`
- `introduced_in`, `removal_gate`
- `usage_signal`, `repository_callers`
- `rollback_release`
- `external_migration_evidence` or explicit `not_applicable`
- `status`: `inventoried`, `bridged`, `migrated`, `eligible-for-removal`, `removed`

State transitions:

```text
inventoried -> bridged -> migrated -> eligible-for-removal -> removed
                    \-> bridged (rollback)
```

Rules:

- Exactly one canonical target.
- No second implementation or authority.
- Removal requires two consecutive zero-caller snapshots plus external gate.
- Unknown external use blocks removal until the user explicitly approves a
  documented migration/expiry decision; repository scans alone are insufficient.

## CandidateIdentity

Immutable validation lineage.

Fields:

- source and dependency digests
- build/release/image/SIF digests or explicit immutable
  `DEFERRED_TO_SPEC110` markers; Spec 111 candidates MUST use the markers and a
  later Spec 110 runtime candidate supplies new concrete digests
- plan, model/tokenizer, policy and configuration digests
- runtime/backend versions
- evidence epoch and result directory

Rules:

- Historical identities and evidence files are immutable.
- Any post-separation build/execution receives a new identity.
- Evidence cannot be promoted across identities.

## Relationships

```text
CoreExecutionPlan 1 -> N CandidateSnapshot
OptimizationObjective 1 -> N OptimizationDecisionEvidence
EngineSnapshot 1 -> N policy least-input projections
ModelAlternativeSet 1 -> 1 ModelVariantCandidateSet
ModelVariantCandidateSet 1 -> N variant-bound PlanCandidateSet
EngineDecisionGraph 1 -> N DecisionPort
DistributedInferenceEngineConfiguration 1 -> 1 EngineDecisionGraph
DistributedInferenceEngineConfiguration 1 -> 1 OptimizationSuite
DistributedInferenceEngineConfiguration 1 -> 1 ExecutionAdapterRegistry
DistributedInferenceEngineConfiguration 1 -> 0..1 OptimizationObserver
PlanCandidateSet 1 -> N CoreExecutionPlan
CandidateSnapshot 1 -> N ProviderAssignmentDecision
ExecutionTargetCandidateSet N -> N ProviderAssignmentDecision
ProviderAssignmentDecision 1 -> 0..1 accepted AssignmentContext
ProviderAssignmentDecision 1 -> 0..1 ValidatedExecutionIntent
ValidatedExecutionIntent 1 -> 1 RequestCoordinatorBinding
ValidatedExecutionIntent 1 -> N AuthenticatedProviderReceipt
ValidatedExecutionIntent 1 -> 0..1 ExecutionCommitCertificate
ExecutionCommitCertificate 1 -> N AuthenticatedProviderReceipt
ExecutionCommitCertificate 1 -> 1 ResultRendezvousRecord
DeploymentDefinition 1 -> N DeploymentRevision
DeploymentRevision 1 -> N DeploymentInstanceRecord
DeploymentRevision 1 -> N ArtifactReference
DeploymentRevision 1 -> N PreparedPlanSession
DeploymentRevision 1 -> N InferenceRequestHandle
DeploymentOperationHandle N -> 1 DeploymentRevision
RuntimeJournal 1 -> N DeploymentRevision
RuntimeJournal 1 -> N DeploymentOperationHandle
RuntimeJournal 1 -> N InferenceRequestHandle
RuntimeJournal 1 -> N RequestEnvelopeReference metadata records
InferenceRequestHandle 1 -> 1 RequestEnvelopeReference
RuntimeAllocationHandoff 1 -> 1 DeploymentRevision
RuntimeAllocationHandoff 1 -> 1 immutable OCI/SIF release
RuntimeAllocationHandoff 1 -> 1 allocation process map
InfrastructureAllocationHandle N -> 1 RuntimeAllocationHandoff
InfrastructureAllocationHandle 1 -> 0..1 DeploymentOperationHandle
InferenceRequestHandle 1 -> 0..1 ExecutionCommitCertificate
InferenceRequestHandle 1 -> 1 ResultRendezvousRecord
DeploymentLifecycleDecision 1 -> 0..1 DeploymentLifecycleRecord
DeploymentLifecycleRecord 1 -> 0..1 committed action certificate
Provider boot epoch 1 -> N OrphanCleanupRecord
AssignmentContext 1 -> N execution evidence records
ProviderAssignmentPolicyDescriptor 1 -> N ProviderAssignmentDecision
OptimizationSuite 1 -> N OptimizationExtensionDescriptor
ExecutionAdapterRegistry 1 -> N ModelAdapterDescriptor
OptimizationExtensionDescriptor 1 -> N OptimizationDecisionEvidence
DecisionBudget 1 -> 1 OptimizationDecisionEvidence
DecisionPort 1 -> N OptimizationDecisionEvidence
ModelAdapterDescriptor N -> N CoreExecutionPlan
ExecutionTargetCandidateSet N -> N ModelAdapterDescriptor
DeploymentLifecycleDecision N -> 1 OptimizationDecisionEvidence
SchedulingDecision N -> 1 OptimizationDecisionEvidence
ExecutionTuningDecision N -> 1 OptimizationDecisionEvidence
CacheDecision N -> 1 OptimizationDecisionEvidence
ValidatedExecutionIntent 1 -> N execution/progress evidence records
ProgressCheckpoint N -> 1 request attempt
OptimizationObserver 1 -> N OptimizationOutcome
CompatibilityEntry N -> 1 canonical owner surface
CandidateIdentity 1 -> N plans/contexts/evidence records
```
