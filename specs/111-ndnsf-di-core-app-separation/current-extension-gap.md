# Current API Gap: External Inference Optimization

**Code snapshot reviewed**: 2026-07-14 working tree before Spec 111 implementation

## Bottom line

Current NDNSF-DI is partially extensible, but an independent optimization team
cannot yet replace the complete optimization path through one stable public API.
They can supply split-planner handlers, an ordinary ACK selector and Python
inference handlers, but multi-role Provider assignment, deployment selection,
Runtime v1 placement/scheduling/resource/cache/recovery, backend selection,
model-variant choice, engine orchestration, objective/snapshot lineage, plugin
lifecycle, atomic decision application, online outcome feedback and native
out-of-tree consumption are fragmented or internal. Existing generic leases
already provide prepare/commit/activate/expiry primitives, but do not yet provide
complete authenticated receipt certification, requester-crash recovery,
deployment lifecycle fencing or periodic idle orphan reclamation.
The public deploy/use path is also incomplete: `APPDeployment` is currently a
read-only definition façade, `deploy_plan()` prepares static metadata rather
than running Providers, and process-local Futures cannot be reopened after an
APP restart.

## Current versus required

| Optimization area | Current usable seam | Current limitation | Spec 111 target |
| --- | --- | --- | --- |
| Model partition/layout | `PlannerBackendRegistry.register(PlannerBackend(handler=...))`; callers may construct `SplitterOutput` | Registry is manually assembled by examples, not selected through APPClient; no installed-extension lifecycle/test kit | Public `PartitionPlanner`, instance registry, standalone wheel and APP suite selection |
| Model variant | Callers/config select an exact model manifest, Qwen size/precision or adapter | No owner for quality-constrained alternatives; selecting one model before partition/provider feasibility blocks joint optimization | Conditional `ModelVariantPolicy` returns bounded authorized candidates; assignment selects the final variant-bound plan tuple and exact constraints become singleton |
| Deployment selection | Deployment discovery sorts ACTIVE/IDLE/DISK/EVICTED records and helpers activate/reserve deployments | Ordering and choice are internal; no idempotent readiness/cooldown/residency/drain contract for scaling/unload | Public control-plane/session `DeploymentPolicy` with lifecycle preconditions; current behavior is a versioned default |
| Ordinary replicated-service Provider assignment | `ServiceUser.request_service_select(..., selector=...)` | One ad-hoc callback exists, while built-ins are not one versioned DI policy surface | One-role/multiplicity form of public `ProviderAssignmentPolicy`; current First/Random/All choices become defaults |
| Multi-role Provider assignment | `request_collaboration(..., ack_observer=...)` exposes candidate observation | Observer cannot select; native `RoleAssignmentSelectionPolicy` performs assignment and reads ambient preferences | Explicit-role form of the same `ProviderAssignmentPolicy`; request-scoped decision passed to native collaboration |
| Assignment cost/objective | Runtime v1 public-ish functions can be imported and called | Weights/objective and selection functions are implementation internals; APPClient has no injection point | Unified versioned assignment policy, Core validation and explicit suite injection |
| Scheduling/dispatch | Runtime v1 role/transfer queues, Provider worker FIFO and Qwen whole-generation scheduler combine fixed ordering with worker/window bounds | Cross-role DAG and Provider-local batch scheduling are conflated; no adapter-declared in-flight batch capability | One scoped `SchedulingPolicy`: APPClient request-DAG dispatch and APPProvider local capability-aware batching |
| Per-invocation tuning | Segment/microbatch/compression choices are internal functions | No stable typed/ranged hook for transfer/prefetch/overlap/speculative parameters | `ExecutionTuningPolicy` over declared `TuningParameterSpec`; it cannot dispatch or resize worker/queue capacity |
| Cache decisions | Runtime v1 exact/KV cache and experimental semantic cache contain reuse, ranking, admission, state placement, eviction and compute-Provider selection | Cache state management, decision phase and final compute assignment are mixed | One `CachePolicy` with closed phase/action kinds owns state actions/affinity; final compute Provider remains assignment |
| Engine composition | No `DistributedInferenceEngine` symbol; responsibilities are distributed across client, APP, deployment, Runtime v1 and provider/native workers | No objective units/uncertainty, atomic multi-decision commit, feedback or state lineage | APP-owned process-local engine with metric/estimate contracts, atomic `ValidatedExecutionIntent` and optional outcome observer |
| Distributed execution/deployment consistency | `ProviderExecutionLeaseTable`, authenticated Targeted lease service, `DistributedLeaseTransaction` prepare-all/commit-all and `ExecutionAttemptAuthority` | Commit responses are reduced to lease state rather than retained authenticated receipts; no exact commit certificate gates activation; requester crash, lifecycle single-writer fencing and idle periodic cleanup are not complete contracts | Reuse canonical leases; requester coordinates one attempt; exact authenticated `ExecutionCommitCertificate` gates Selection/activation; attempt/boot/lifecycle epochs fence stale actors; TTL plus periodic Provider cleanup bounds orphans |
| Operator deployment workflow | `APPDeployment` loads/generates a deployment definition; Runtime v1 exposes separate status/doctor utilities | No immutable resolved revision, apply/wait/rollback/drain/delete API, durable operation state, revision-scoped READY/ACTIVE proof or one canonical startup/shutdown workflow | `DeploymentDefinition` resolves to immutable `DeploymentRevision`; APPDeployment reconciles it through durable operation handles and a fixed APP RuntimeJournal |
| Application invocation workflow | `DistributedInferenceClient.deploy_plan()` publishes/caches metadata; synchronous and async/Future inference paths exist | The method name implies deployment although it does not activate Providers; accepted requests have no durable reopen/status/cancel/result handle across process restart | Rename canonical metadata preparation to `prepare_session()` with bounded aliases; use journal-backed `InferenceRequestHandle` for submit/open/status/wait/result/cancel/stream |
| iTiger runtime handoff | OCI/SIF release helpers, one-workload Slurm adapter, fixed-three allocation topology v1 and a single-node all-process Qwen launcher exist | No post-Spec-111 candidate/revision handoff, persistent state/node-run binds, revision-derived Provider count, same-SIF guarantee for topology commands or scheduler/APP/request state separation | Operations-owned immutable `RuntimeAllocationHandoff` and v2 process map; Spec 110 builds a new OCI/SIF and validates single-node before selected-transport multi-node |
| Admission | `APPProvider.serve_service(..., admission_policy=ProviderAdmissionPolicy(...))` | Only provider threshold; no engine tenant/quota/backpressure gate | One scoped hook: `ENGINE_REQUEST` then `PROVIDER_LOCAL`; either may only tighten floors |
| Recovery/fallback | Runtime v1 bounded replan, retry and generated fallback choices are fixed | Recovery embeds replacements and lacks visible-output/checkpoint boundaries | Transition-only `RecoveryPolicy` over Core-advertised restart/resume boundaries; owning policies produce replacements |
| Execution target | Runtime compatibility picks the first backend and ONNX selects provider/device/CPU fallback internally; planner lookup is explicit registry resolution | Target optimization, Provider feasibility, planner configuration and runner creation are conflated | `ExecutionTargetPolicy` proposes role/Provider-compatible targets; assignment selects the joint tuple, adapter registry creates it, planner lookup stays configuration |
| Python inference engine | `APPProvider.serve_service(..., handler=callable)` | Usable but not registered/versioned as an engine adapter; no common contract kit or package identity | Public `RunnerAdapter` in `ExecutionAdapterRegistry`, separate from `OptimizationSuite` |
| Native C++ engine | `NativeModelRunnerFactory`, `RegistryNativeModelRunnerFactory`, `NativeProviderRuntime.registerRunner` | Valid internal C++ seam, but installed public-header/out-of-tree build path is not accepted as a product surface | Installed public runner SDK plus out-of-tree sample build/link gate |
| Package discovery | Python imports/manual registry construction | No explicit allowlist/version/digest policy; automatic discovery would be unsafe | Direct registration first; opt-in allowlisted entry-point loader owned by APP |
| Reproducibility/failure | Individual tests/logs | No common seed/budget/digest/timeout/fallback evidence contract | Shared descriptor, `DecisionBudget`, typed evidence and replay contract |
| Online learning/evaluation | Logs and experiment-specific post-processing | No stable decision-to-outcome linkage, state epoch or failure-isolated feedback | Optional independent `OptimizationObserver` over bounded outcomes; APP/extension owns state |
| Decision consistency/privacy | Sequential internal calls and broad application objects | Individually valid decisions can become torn; external code could receive excess payload | Atomic Core/provider intent commit plus least-input projections and tenant-scoped cache/outcome facts |

## Concrete code evidence

- `planner_registry.py:25-206` defines handler-based planner registration, but
  `default_planner_registry()` is intentionally empty and model/examples create
  their own registries.
- `app.py:241-856` exposes APPClient plan/inference façades without a planner,
  placement or optimization-suite constructor argument.
- `client.py:142-760` calls `ServiceUser.request_collaboration()` directly and
  does not pass a role-selection policy.
- `pythonWrapper/ndnsf/service.py:1100-1138` supports a selector for ordinary
  service requests; `service.py:1193-1251` gives collaboration only an
  observational ACK callback.
- `pythonWrapper/src/ndnsf/_ndnsf.cpp:1563-1680` implements multi-role
  `RoleAssignmentSelectionPolicy` internally.
- `runtime_v1.py:1399-1930` embeds Runtime v1 placement scoring, assignment and
  preset strategies; later sections embed exact-cache eviction, role/transfer
  scheduling, retry/fallback, segment/microbatch and KV-cache placement choices.
- `qwen_pilot.py:33-119` fixes generation queue behavior; `onnx_graph.py`
  ranks split candidates; `runtime_compatibility.py` selects the first compatible
  backend; `deployment.py` orders discovered deployments internally.
- `experimental/semantic_cache/implementation.py` embeds pattern ranking,
  admission thresholds, eviction and cache-aware Provider selection.
- `app.py:921-1015` and `provider.py:347-796` permit an application-supplied
  Python inference handler and a fixed threshold admission policy.
- `NativeModelRunner.hpp:24-88` and `NativeProviderRuntime.hpp:14-44` provide
  useful native interfaces, but current Python bindings do not expose a complete
  external native-runner installation/registration path.

## Meaning for the separate algorithm team

Today they can prototype by importing internals or manually wiring handlers, but
that couples their work to Runtime v1 files, example registries and current C++
binding behavior. They cannot honestly claim to consume a stable optimization
SDK until the complete decision-point inventory, ten-policy external wheel,
independent Runner adapter and optional observer SPIs, default parity, APP
injection, unified ordinary/multi-role assignment, joint model/plan/Provider/
target selection, certificate-gated distributed intent, scoped admission/
scheduling, least-input and out-of-tree runner gates pass. The normative
coverage is in `contracts/python-optimization-surface.md` and
`contracts/distributed-execution-consistency.md`. Operational deployment is
not complete until the validate/resolve/dry-run/apply/READY/ACTIVE/request-
reopen/drain/delete workflow in
`contracts/deployment-and-invocation-workflow.md` passes its clean MiniNDN gate.
