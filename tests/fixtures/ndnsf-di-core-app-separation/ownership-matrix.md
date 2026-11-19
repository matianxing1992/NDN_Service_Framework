# Contract: Ownership Matrix

| Concern | Canonical owner | Allowed dependencies | Forbidden ownership/import |
| --- | --- | --- | --- |
| NDN invocation, security, generic telemetry/leases | NDNSF Core | ndn-cxx/NFD/ndn-svs/security dependencies | DI model or APP policy |
| Plan/assignment validation, atomic execution-intent commit/abort, dependency execution, attempt/recovery/cache binding | NDNSF-DI Core | NDNSF Core, native runner/dependency interfaces | APP policy, planner/model implementation, GUI, experiment or operations implementation |
| Per-request prepare/commit/abort coordination and complete receipt-certificate assembly | Initiating APPClient requester identity | Existing authenticated Targeted lease service, Core validation and Provider receipts | New coordinator service, cluster-global election, Provider-local capacity authority or cross-identity takeover |
| Provider-local lease receipt, boot-epoch fencing, certificate-gated activation and orphan cleanup | APPProvider / NDNSF Core lease mechanism | Canonical generic lease table, Provider clock/boot epoch and validated certificate | Policy-created authority, incomplete receipt activation or APP-owned local cleanup truth |
| Deployment lifecycle action stream, lifecycle fencing and action certificate | Operator-authorized APPDeployment identity | Core contract, Provider compare-and-set and authenticated receipts | Multiple implicit writers, automatic leader election or destructive best-effort action |
| Deployment definition validation, immutable revision, apply/status/wait/rollback/drain/delete and reconciliation | APPDeployment | Existing deployment configuration, Core contracts, lifecycle certificates and fixed RuntimeJournal | CLI/Core lifecycle logic, second manifest or metadata session presented as deployment |
| Durable request submit/open/status/wait/result/cancel/stream | APPClient | Engine, Core request/certificate/rendezvous, fixed RuntimeJournal and protected request-envelope spool/reference | Process-local Future as authority, plaintext journal input, Provider-owned client recovery or CLI inference logic |
| Revision-bound artifact staging, readiness and local drain/cleanup | APPProvider | External ArtifactReference, adapter/runtime, permissions, boot/revision epoch and active bindings | Liveness-only readiness, embedded model weights or deployment policy authority |
| APP lifecycle/request restart evidence | Fixed APP-owned RuntimeJournal plus owner-only protected request spool | Mounted local persistent filesystem or durable NDN envelope repository, authenticated evidence references and bounded retention | Optimization state, plaintext/secret journal storage, public state-store SPI, consensus or cluster database |
| Provider process/container/job provisioning | External operator and Docker/Slurm/systemd adapters | Owner profiles, identities, mounts and generic APPProvider entrypoint | Core scheduler, APPDeployment hidden OS supervisor or revision readiness before apply |
| iTiger OCI-to-SIF release and allocation launch | Existing operations-owned Slurm/Apptainer adapter / Spec 110 | Immutable runtime-allocation handoff, Slurm, Apptainer, process map and selected network probe | Docker daemon on iTiger, APP/Core scheduler or pre-Spec-111 candidate relabeling |
| Infrastructure allocation status | `InfrastructureAllocationHandle` / operations adapter | Slurm job ID/state plus immutable handoff digest | READY/ACTIVE or request-success inference from PENDING/RUNNING/COMPLETED |
| Deployment, optional model-variant candidate proposal, split, unified Provider assignment and SLO objectives | Python optimization SDK / Planner / APP | DI Core contracts, authorized model alternatives and Core-provided facts | Exact-constraint replacement, authority, token, lease or stale-state bypass |
| Process-local decision graph, objective/snapshot projection and policy invocation | APP-owned `DistributedInferenceEngine` | Optimization SDK, APP executors, Core ports/validators and adapter registry | Network coordinator/service, process-global singleton, persistence or security authority |
| Mechanism-facing decision ports and validators | NDNSF-DI Core | immutable Core facts/contracts | SDK, APP or model implementation imports |
| External optimization protocols/test kit | Optimization SDK | re-exported DI Core ports/contracts only | Core execution internals, APP implementation, model implementation |
| Deployment registry/executor/default selection | APPDeployment | Optimization SDK and exact operator constraints | Core-owned discovery or policy |
| Client-side registry/discovery/executor/fallback, engine admission, model candidates and request-DAG scheduling | APPClient | Optimization SDK, explicit operator configuration and authorized alternatives/assignments | Core-owned discovery, implicit/global loading, cross-scope queue mutation or exact-model replacement |
| Provider-side registry/executor/fallback, provider admission and provider-local batching | APPProvider | Optimization SDK, adapter capabilities and explicit provider configuration | Core-owned plugin lifecycle, cross-role assignment or safety-floor bypass |
| Provider assignment, scoped scheduling/dispatch, execution tuning, cache, scoped admission, recovery and execution-target objective | Python optimization SDK / Planner / APP | Core/provider actionable snapshots, objective, adapter capabilities and typed bounds | Creating readiness/validity/authority, cross-scope mutation or weakening safety floors |
| Objective schema and engine snapshot lineage | DI Core structural contracts + APP assembly | Workload-neutral fields, adapter-declared quality identity and telemetry lineage | Policy upgrading stale/ineligible facts or Core interpreting model-family quality semantics |
| Cache/session binding and eviction safety | NDNSF-DI Core | Plan/state identity and runtime storage | Workload placement objective |
| Cache-object placement/reuse/affinity objective | Planner/APP | Core cache telemetry and immutable state facts | Final compute Provider assignment or Core hard-coding one workload objective |
| Runner adapter registration/creation | ExecutionAdapterRegistry / model adapter | Selected execution target and public runner contracts | OptimizationSuite ownership, registration-as-selection or authority bypass |
| Outcome feedback and optimizer learning state | Optional APP-owned `OptimizationObserver` / extension | Bounded outcome records, state epoch/digest and separate executor | Current-request decision authority, Core plugin database, sensitive payload disclosure or inference blocking |
| APP façades and deployment workflow | APP SDK | Planner interfaces, DI Core | Core importing APP |
| ONNX/Qwen/llama behavior | Model adapters | Planner/runner contracts | Core importing adapter implementation |
| CLI/doctor/status/metrics/launch | Operations | Public owner interfaces | Core importing operations |
| MiniNDN/iTiger campaigns | Experiments/deployment specs | Installed owner interfaces | Installable Core importing campaigns |

Enforcement:

1. Static dependency tests fail on every forbidden import.
2. Core-only import inventory fails when a forbidden owner module loads.
3. Compatibility adapters may import canonical owners but canonical owners never
   import compatibility adapters.
4. No ownership change may add an NDNSF authority or top-level wire name in
   Spec 111. Versioned fields required by the consistency certificate may extend
   only existing lease, Selection and result payloads and fail closed on version
   mismatch.
5. External packages import public SDK modules only; Core never imports or
   discovers an external package.
6. Registries and suites are instance-scoped; tests forbid module-global and
   environment-selected optimization state.
7. Entry-point allowlisting verifies identity only. Extension code remains
   operator-trusted application code; worker processes are containment, not a
   security sandbox.
8. Every policy-bearing decision appears in `contracts/python-optimization-surface.md`
   and the machine-readable inventory; new unclassified sites fail CI.
9. Current policy algorithms implement the same ten public Python policies as
   external algorithms and are selected only through
   `DefaultOptimizationSuite`; runner implementations use the same independent
   adapter SPI through `DefaultExecutionAdapterRegistry`; outcome observation is
   a second optional non-policy SPI.
10. Placement/load balancing remain Provider assignment; parallel layout
    remains partitioning; scaling/model residency remain deployment;
    communication parameters remain tuning; memory is divided among deployment,
    cache, tuning and Core feasibility rather than duplicated in new policies.
11. APP prepares decisions, but Core/provider mechanisms expose execution only
    after the requester coordinator presents a complete authenticated commit
    certificate. Prepare/commit alone never activates work; abort releases
    reachable reservations and lease expiry plus periodic cleanup bounds the rest.
12. Request attempt epoch, Provider boot epoch and deployment lifecycle epoch
    are independent fencing domains. A timeout or network partition cannot grant
    a new requester or deployment writer authority.
13. `DistributedInferenceDeployment` is the one compatibility definition input;
    immutable revisions resolve from it. `DeploymentSession`/`deploy_plan()` are
    metadata-only aliases and never count as lifecycle activation evidence.
14. Operations commands render/call APP operations. Static tests reject any
    CLI-owned readiness, revision selection, lifecycle transition, request
    coordination or result-authority implementation.
15. RuntimeJournal durability makes APP operations reopenable but cannot replace
    Provider receipts, security validation, leases, certificates or rendezvous
    authenticity.
16. External infrastructure launches generic Provider agents. APPDeployment may
    select them and apply revision/model lifecycle actions only after their
    boot-epoch capability registration; it never claims to provision a process.
17. A reopenable request exists only after its protected wire envelope and
    digest-bound journal reference are durable. Plaintext payload never enters
    journal/status/metrics, and missing/tampered input fails closed.
18. Runtime state is partitioned by canonical application/NDN owner identity and
    deployment/request stream. Traversal, symlink and cross-tenant reads fail;
    different identities exchange only authenticated protocol references.
19. Every iTiger project command runs inside the exact verified SIF. Host
    supervision is limited to Slurm/Apptainer, allocation routes and finalization;
    Docker and host project executables are not runtime dependencies.
20. Infrastructure, deployment and request handles remain distinct. No Slurm
    state grants APP or Core authority.
