# Spec 111 Audit — Engine, Distributed Consistency, Operational Workflow, and iTiger Handoff Readiness

**Audit date**: 2026-07-14
**Mode**: code-aware pre-implementation, repaired in place by explicit user request
**Revision**: joint model/plan/Provider decisions, scoped control, distributed
execution/deployment consistency, metric/estimate semantics, optional feedback,
revision-bound deployment/invocation operations and the OCI-to-SIF/Slurm iTiger
handoff
**Verdict**: **PASS (pre-implementation design)**
**Open findings**: **0 CRITICAL, 0 HIGH, 0 MEDIUM, 0 LOW**

## Findings repaired in this revision

| ID | Prior severity | Finding | Repair |
| --- | --- | --- | --- |
| R1 | HIGH | Model and execution target were selected outside partition/Provider feasibility, preventing true joint optimization. | Model and target policies now propose bounded authorized/compatible candidates; assignment selects the final variant/plan/Provider/target tuple. |
| R2 | HIGH | Independently valid policy results could be applied under different snapshot/reservation epochs, producing torn execution state. | Added Core/provider `ValidatedExecutionIntent` prepare/revalidate/commit-or-abort with release and fault-injection acceptance. |
| R3 | HIGH | External algorithms had decision input but no stable execution-outcome feedback or state lineage. | Added optional off-path, idempotent `OptimizationObserver` SPI and `policy_state_epoch`/`policy_state_digest`; persistence remains APP/extension-owned. |
| R4 | MEDIUM | Cross-role DAG scheduling and Provider-local continuous batching had one owner and no scope isolation. | Kept one `SchedulingPolicy` with `REQUEST_DAG` and `PROVIDER_LOCAL` scopes and separate APPClient/APPProvider ownership. |
| R5 | MEDIUM | Admission covered only Provider thresholds, omitting request-level quota/SLO/backpressure before expensive planning. | Kept one `AdmissionPolicy` with `ENGINE_REQUEST` and `PROVIDER_LOCAL` scopes; either rejection is binding and floors cannot be weakened. |
| R6 | MEDIUM | Objective weights lacked unit, direction, aggregation, normalization and missing-data semantics; forecasts were indistinguishable from observations. | Added `ObjectiveMetricSpec` semantics and `EstimateEnvelope` with confidence, horizon, source and freshness. |
| R7 | MEDIUM | The batch contract assumed token-phase equality and deployment scaling lacked idempotency/cooldown/drain safety. | Batch compatibility now comes from Runner `BatchCapability`; lifecycle actions carry state epoch, idempotency, readiness, residency, hysteresis and drain preconditions. |
| R8 | MEDIUM | Streaming recovery, exact decoding semantics, candidate explosion and least-input/multi-tenant privacy were underspecified. | Added progress/output/checkpoint epochs, exact request-semantics constraints, deterministic cardinality budgets and sensitive/cross-tenant data exclusion. |
| R9 | HIGH | Local `ValidatedExecutionIntent` atomicity did not define who coordinates a multi-Provider request, whether partial commit could activate work, or what survives requester crash/partition. | Reused the existing authenticated lease transaction, made the initiating requester identity the per-attempt coordinator, and required an exact authenticated `ExecutionCommitCertificate` before Selection/activation. |
| R10 | HIGH | Deployment lifecycle had no explicit single writer/fencing, and orphan cleanup depended on best-effort messages or future operations. | Added APPDeployment owner/lifecycle-epoch CAS and action certificates, Provider boot/attempt/lifecycle fencing, phase-specific crash/partition semantics and bounded periodic idle-time reclamation. |
| R11 | MEDIUM | Reclaiming a resource could also erase its highest accepted epoch, allowing a delayed stale operation to regain authority after cleanup. | Separated resource reclamation from bounded high-watermark tombstone retention; new prepare binds expected Provider boot epoch and tests replay before/after tombstone expiry. |
| R12 | HIGH | `APPDeployment` exposed definition accessors but no validate/apply/status/wait/rollback/drain/delete workflow, so a user could not operate the designed deployment lifecycle through one public APP API. | Added a canonical definition-to-immutable-revision workflow, durable deployment operation handles, idempotent reconciliation and the complete APPDeployment lifecycle API. |
| R13 | HIGH | `DistributedInferenceClient.deploy_plan()` sounded like a deployment operation although it only published/cached static metadata, leaving no canonical deployable identity or revision-bound READY/ACTIVE state. | Renamed the canonical metadata operation to `prepare_session()`/`PreparedPlanSession`, retained bounded aliases, and made `DeploymentRevision` the sole deployable identity with signed readiness closure. |
| R14 | HIGH | Requester-crash and lifecycle recovery required durable evidence, but the design did not choose an owner or minimal persistence mechanism; process restart could lose operation/receipt/rendezvous state. | Added one fixed APP-owned append-only RuntimeJournal with locking, checksums, versioning, fail-closed recovery and bounded compaction; it is mechanism, not a policy SPI or cluster database. |
| R15 | MEDIUM | Async inference returned a process-local Future with no stable request identity, reopen/status/cancel/result semantics or one cross-restart terminal-result rule. | Added journal-backed `InferenceRequestHandle` operations and defined sync/Future calls as compatibility adapters over the same request state machine. |
| R16 | MEDIUM | Startup, readiness, rolling upgrade/rollback, drain/shutdown and operations CLI semantics were scattered or absent, allowing profiles to build without proving that a deployment can actually be operated. | Added ordered bootstrap, revision-scoped READY/ACTIVE evidence, epoch-fenced upgrade/rollback, graceful drain/shutdown, thin CLI parity and a blocking clean-profile MiniNDN validate-to-delete gate. |
| R17 | HIGH | A durable request handle could survive restart while its inline prompt/input existed only in dead process memory; storing plaintext input in the journal would violate the least-input/security boundary. | Added a fixed owner-only protected request-envelope spool or durable NDN repository: existing authenticated/confidentiality-protected wire bytes persist first, while the journal stores only name/digest/security/expiry; missing/tampered evidence fails closed. |
| R18 | MEDIUM | Startup required an APPProvider already bound/warmed to the revision before `apply()` selected and applied that revision, creating a bootstrap cycle and obscuring process-provisioning ownership. | External container/Slurm/systemd/operator starts generic Provider agents; `apply()` selects eligible agents and performs revision-scoped stage/warm/readiness. APPDeployment is explicitly not an OS/container/job scheduler. |
| R19 | MEDIUM | DRAFT/VALIDATED/RESOLVED, running phases, FAILED and DEGRADED were modeled as one linear lifecycle even though an ACTIVE deployment may also be degraded and a partition is unknown rather than inactive. | Separated definition/revision state, instance phase and reason-coded Ready/Active/Degraded/Failed/Unknown/Reconciling conditions; wait/status preserve last verified phase and fail closed on uncertainty. |
| R20 | MEDIUM | One mounted state root could be misread as one shared trust domain, allowing APPDeployment and multiple requester identities to collide with or inspect each other's journal/spool paths. | Partitioned state by canonical application/NDN owner identity and deployment/request stream; identity mismatch, traversal, symlink and cross-tenant access fail before reads, while cross-owner coordination uses authenticated protocol references only. |
| R21 | HIGH | “Use Docker on iTiger” could be interpreted as requiring a Docker daemon or exposing a long-lived IP service, neither of which matches the scheduler-owned compute contract. | Made OCI the sealed build source and exact Apptainer SIF inside Slurm the only iTiger runtime; bounded batch/interactive use is supported, persistent public service is explicitly excluded. |
| R22 | HIGH | The current Spec 110 image predates the unimplemented Spec 111 APP/Core workflow, so reusing its digest would falsely promote substrate evidence to post-separation deployment evidence. | Required a new source/candidate/OCI/SIF and immutable runtime handoff after Spec 111 completion; historical jobs/releases remain read-only substrate lineage. |
| R23 | HIGH | Spec 111 correctly kept APPDeployment out of OS/process scheduling but did not define the bridge from an allocated Slurm job to generic Provider agents and then APP apply/readiness. | Added an operations-owned `RuntimeAllocationHandoff` and distinct infrastructure handle with explicit infrastructure -> generic-agent -> APP apply/READY/ACTIVE -> request -> drain ordering. |
| R24 | HIGH | The current canonical Apptainer runner binds models/artifacts/identity/scratch but no persistent identity-partitioned Spec 111 state root or shared node-local NFD run directory. | Required explicit `/state` and node-run binds, read-only release/model/artifact/role identity, no broad writable project/cross-role access, and corresponding negative tasks in Spec 110. |
| R25 | HIGH | Allocation topology v1 fixes exactly three Providers and generated project commands may resolve from the host; this cannot represent external partition policies or guarantee same-SIF execution. | Added a versioned v2 process map derived from the immutable revision, preserved v1 evidence, required every project command through the exact SIF and bound per-Provider GPU UUID visibility. |
| R26 | MEDIUM | Slurm PENDING/RUNNING/COMPLETED could be conflated with deployment READY/ACTIVE or request success. | Separated `InfrastructureAllocationHandle`, `DeploymentOperationHandle` and `InferenceRequestHandle` states and added scheduler/preemption/drain mutation tests. |
| R27 | MEDIUM | Multi-node launch and external remote use lacked an explicit boundary for selected transport, persistent routes/listeners and post-allocation reachability. | Kept single-node first, gated multi-node on the exact selected-transport probe, prohibited persistent faces/listeners/login daemons and scoped remote use to Slurm or authorized interactive allocation. |
| R28 | HIGH | Spec 111 still mentioned container build/runtime validation even though paying that cost before Core/APP and MiniNDN maturity adds no separation evidence and risks accidental iTiger execution. | Split validation into local unit/native/package/static checks and MiniNDN-only distributed acceptance; deferred every OCI/SIF build, container-runtime invocation and iTiger/Slurm action to the new Spec 110 candidate. |

## Accepted surface and Occam boundary

The accepted optimization surface remains exactly ten policies:

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

There are exactly two independent non-policy SPIs:

- `RunnerAdapter.create` constructs an already-selected execution target and
  advertises device/batch/KV/checkpoint capability.
- optional `OptimizationObserver.observe` consumes a bounded outcome after the
  current decision/result and has no current-request authority.

No separate solution-selection, global/local scheduling, engine/provider
admission, autoscaling, transaction, state-store, estimator, resource,
placement, load-balancing, parallelism, communication, memory, speculative or
backend policy is added. Joint solvers may live inside one suite and implement
several typed ports; distributed certification/activation is mechanism; state and estimation remain
suite-owned until independent interoperability justifies another SPI.

## Ownership and decision flow

```text
APP objective + lineage-bound snapshot
  -> independent control/session DeploymentPolicy updates a later snapshot
  -> AdmissionPolicy(ENGINE_REQUEST)
  -> ModelVariantPolicy.propose or exact singleton
  -> PartitionPlanner: variant-bound plans
  -> ExecutionTargetPolicy.propose per plan-role-Provider
  -> CachePolicy: pre-assignment affinity
  -> ProviderAssignmentPolicy: variant + plan + Providers + role targets
  -> existing Provider leases PREPARE-all / revalidate / COMMIT-all
  -> requester builds complete authenticated ExecutionCommitCertificate
  -> Provider certificate-gated Selection/activation
  -> SchedulingPolicy(REQUEST_DAG)
  -> AdmissionPolicy(PROVIDER_LOCAL)
  -> SchedulingPolicy(PROVIDER_LOCAL)
  -> ExecutionTuningPolicy
  -> RunnerAdapter -> Core/native execution
  -> progress/output-commit/checkpoint
  -> CachePolicy post-action or RecoveryPolicy transition
  -> optional asynchronous OptimizationObserver
```

Deployment is an independent control/session epoch, not an unconditional
request predecessor; bounded no-feasible re-entry creates a new snapshot.
APPClient owns engine admission, candidates, partition/assignment and
request-DAG scheduling. APPProvider owns Provider-local admission/batching,
tuning, cache application and runner creation. APPDeployment owns lifecycle.
Core/provider mechanisms own validation, security, leases, attempt/output
epochs and activation. The initiating APPClient identity coordinates only its
request attempt; APPDeployment is the configured single writer for its
deployment lifecycle stream. The process-local engine is not an NDN service,
cluster-global coordinator, consensus system, security authority, persistence
engine or singleton.

## Code reality

CodeGraph was current: 2,341 files, 50,736 nodes and 167,899 edges. Direct source
verification confirms:

| Current source | Verified fact | Design consequence |
| --- | --- | --- |
| `app.py:241` and `client.py:303` | `APPClient` exists; `deploy_plan` only publishes/caches static metadata. No `DistributedInferenceEngine` symbol exists. | Engine, lifecycle optimization and atomic intent remain proposed, not implemented. |
| `app.py:861-919` | `APPDeployment` wraps `load_or_generate_deployment()` and exposes definition/policy/role/model accessors, but no operational lifecycle methods. | A deployable revision and APP lifecycle API must be added rather than inferred from definition loading. |
| `client.py:303-335` | `deploy_plan()` returns a metadata `DeploymentSession`; async inference uses a process-local Future path. | Metadata preparation and runtime deployment must be named separately; accepted requests need durable handles for restart recovery. |
| `runtime_v1.py` CLI/status adapters | Runtime v1 has status, metrics and doctor utilities but no canonical validate-to-delete deployment interface. | Operations commands must remain thin APP delegates instead of becoming a competing lifecycle implementation. |
| `packaging/.../lib/adapters/slurm_apptainer.py:45-77` | The generic adapter renders one workload (`/bin/true` by default) and submits it; frozen process-map admission T063 is still open. | Post-Spec-111 use needs an immutable handoff renderer and v2 process-map bridge before any live candidate. |
| `packaging/.../scripts/run-container.sh:51-60` | Exact SIF, `--nv`, read-only release/model/artifact/identity and writable scratch/evidence binds exist; no persistent `/state` or shared node-run bind exists. | Add least-privilege state/run binds and remove direct workload access to durable evidence where finalizer staging suffices. |
| `allocation_topology.py:174-175` and `run-allocation-topology.sh` | v1 requires exactly three Providers; generated scripts execute process commands directly under `srun`. | Preserve v1 fixtures but make v2 cardinality revision-derived and wrap every project process in the canonical SIF runner. |
| `run-ndnsf-qwen.sh` | Single-node launcher can start NFD/controller/providers/user inside one SIF, but does not execute Spec 111 revision/apply/durable-handle semantics. | Useful substrate/orchestration reference only; it cannot close the post-Spec-111 APP acceptance gate. |
| Spec 110 T214-T218 | Job 149669 failed SIF materialization before download; a later itiger07 UID/Apptainer preflight passed, but replacement materialization and GPU probe remain open. | Logical feasibility is not executed SIF/GPU/Qwen evidence; live work remains gated and separately authorized. |
| `runtime_v1.py:2368` | `RolePipelineScheduler` orders dependency-ready provider work with bounded active count. | This is request-DAG parity evidence, not a complete Provider continuous-batching engine. |
| `qwen_pilot.py:33` | `BoundedGenerationScheduler` runs each whole generation as one worker-owned job and tracks monotonic token progress. | Adapter-local in-flight batching and checkpoint semantics require new contracts/tests; current code does not prove them. |
| `ProviderRoleWorker.cpp:427` | Provider work is FIFO; exact-forward cache mutation is embedded around execution. | Provider-local scheduling/cache policy must be extracted while mechanism state/atomicity remains native. |
| `provider.py:55,668` | `ProviderAdmissionPolicy` is a fixed local threshold evaluation. | Provider-local parity can migrate; engine request admission is new capability. |
| `ExecutionLease.hpp/.cpp` | `GenericExecutionLease` and `ProviderExecutionLeaseTable` already own Provider boot epoch, idempotency, conflict keys, prepare/commit/activate/abort/renew/release and expiry. | Spec 111 must extend/reuse this authority; a DI-specific second lease table is forbidden. |
| `deployment.py:623-852` | `DistributedLeaseTransaction` already performs requester-owned prepare-all/commit-all and best-effort cleanup, but returns only a lease set and drops authenticated response proof. | Complete receipt preservation, certificate-gated activation and crash recovery are new proposed work, not an existing guarantee. |
| `NativeProviderHandler.cpp:852` and `NativeExecutionPlan.cpp:59-120` | Native activation validates a lease and attempt authority rejects stale/cancelled/duplicate terminal attempts. Cleanup is callable/on-operation, not yet proven as an idle periodic sweep. | Certificate membership must join the existing activation check; attempt authority remains canonical and periodic orphan cleanup needs an explicit owner/test. |
| negative source scan | No `OptimizationObserver`, `ValidatedExecutionIntent`, `BatchCapability` or policy-state digest symbol exists. | All revised mechanisms are correctly labeled proposed and must not inherit old experimental claims. |

## Research-system calibration

Primary sources support the need for capability- and phase-aware contracts but
do not prove an NDNSF-DI performance benefit:

- vLLM documents separate TTFT/ITL tuning and connector-owned KV transfer for
  disaggregated prefill, while explicitly warning that the mode is not a
  universal throughput improvement.
- TensorRT-LLM documents phase-specific resources, modular KV exchange,
  heterogeneous parallel layouts and in-flight scheduling under KV capacity.
- DistServe jointly optimizes phase resource allocation/parallelism/placement
  under distinct TTFT and TPOT requirements.
- Ray Serve separates queue limits from autoscaling and exposes metric windows,
  aggregation and asymmetric scale delays.

These sources justify typed objectives, estimates, adapter capabilities and
lifecycle stabilization. They do not justify claiming that new batching,
disaggregation, scaling or joint selection is already implemented or faster.

## Security and distributed-correctness audit

- Exact operator/model/tokenizer/prompt/sampling/stop/decoding constraints
  dominate optimization and unauthorized alternatives fail closed.
- Policies receive workload shape, not prompt/tensor/credential/token/decrypted
  policy or unrelated tenant data by default.
- Cache/KV facts and outcomes remain tenant/security scoped; active references
  and leases protect eviction/migration.
- One intent binds model, plan, Providers, target, tuning, cache affinity,
  objective/snapshot and policy-state lineage; prepare/commit remain inert until
  the exact authenticated Provider receipt set forms a commit certificate.
- Attempt epoch, Provider boot epoch and deployment lifecycle epoch fence
  different authority domains and are never substituted for one another.
- Cleanup releases expensive resources first but retains bounded epoch
  tombstones through the maximum operation/result replay window.
- Network partitions fail closed for new authority. Already certified work is
  deadline-bounded; best-effort release accelerates cleanup while Provider TTL
  and periodic sweep are the safety basis.
- Mixed prefill/decode batching is accepted only when the selected adapter
  advertises it; hedges use only assignment-authorized replicas.
- Recovery may choose only Core-advertised checkpoint/output boundaries and
  cannot publish duplicate, reordered or stale visible output.
- Observer timeout/failure is separately evidenced and cannot change inference
  result or Core availability.
- NAC-ABE, permission, one-time tokens, replay, Targeted, provider permission,
  lease, deadline and attempt authority remain canonical. Only versioned fields
  on existing lease/Selection/result payloads may carry receipt/certificate
  evidence; no new top-level NDN name or security authority is introduced.

## Adversarial semantic review

The strongest remaining counter-arguments were tested inline under the ARS
reviewer lens:

1. **“The ten APIs still cannot express one joint optimizer.”** Rejected: one
   package may share a solver internally; candidate lineage and final joint
   assignment expose the coupled solution without a universal untyped callback.
2. **“An observer creates a hidden control loop.”** Rejected only because it is
   one-way, post-decision, separately budgeted and cannot affect the current
   request; later decisions identify the state version they consumed.
3. **“Atomic intent is a hidden global transaction coordinator.”** Rejected
   only after revision: the initiating requester identity coordinates one
   attempt over existing authenticated leases; Providers retain local authority.
   The guarantee is complete-receipt execution visibility plus fencing/expiry,
   not simultaneous commit, consensus or partition availability.
4. **“Advanced serving terms overclaim implementation.”** Controlled: all
   advanced alternatives are contract capability; parity defaults preserve
   current FIFO/whole-generation behavior until executed evidence exists.
5. **“A requester crash after partial commit can still leak or execute work.”**
   Controlled: committed-without-certificate leases are non-executable and
   finite; restart may release them, while periodic Provider cleanup is the
   independent safety basis.
6. **“Two APPDeployment processes with the same identity are both writers.”**
   Controlled: identity is necessary but insufficient; lifecycle epoch,
   expected-state digest and action digest are compare-and-set fencing, with
   conflicting same-epoch actions rejected.
7. **“Certificate publication to only some Providers violates atomicity.”**
   Controlled by the deliberately narrower claim: a subset may perform bounded
   certified work, but incomplete role/result evidence cannot become one visible
   terminal result. The design does not promise failure-atomic machine state.
8. **“A local journal is a hidden distributed database.”** Rejected only under
   the fixed boundary: it is APP-local durable recovery state, uses no network
   consensus or optimization API, and cannot grant Provider authority without
   signed receipts/certificates and existing lease checks.
9. **“A deployment is usable as soon as processes start.”** Rejected:
   activation requires complete fresh revision-scoped readiness evidence;
   startup success, READY and ACTIVE are distinct observable states.
10. **“Rollback can simply reactivate an old record.”** Rejected: rollback
    resolves a new lifecycle epoch (and auditable revision decision), fences
    the failed writer/work and passes the same readiness/certificate gates.
11. **“A durable handle implies its input is durable.”** Accepted as a prior
    defect and repaired: submit cannot return a reopenable handle until the
    existing protected RequestMessage wire envelope and its journal reference
    are durable; plaintext is never journaled.
12. **“APPDeployment starts the Provider containers.”** Rejected for Spec 111:
    infrastructure launches generic agents; APPDeployment operates revision and
    model lifecycle inside those agents. This keeps Docker/Slurm/systemd adapters
    outside Core without leaving readiness ownership ambiguous.
13. **“One persistent volume means all APP identities may share files.”**
    Rejected: the volume is namespaced and owner-protected; operator and
    requester records exchange authenticated references, not filesystem trust.
14. **“The Docker image can just run unchanged on iTiger.”** Rejected: iTiger
    consumes a SIF and injects the host driver through `--nv`; a new post-Spec-
    111 image is required because the current image lacks the new implementation.
15. **“Slurm job launch is APPDeployment.apply.”** Rejected: launch creates
    resources/processes; apply creates revision/model readiness inside live
    agents. Combining them would break local portability and duplicate state.
16. **“The existing three-stage process map is generic enough.”** Rejected:
    external partition/model policies may change role/cardinality; fixed three
    remains a Qwen pilot fixture, not a framework invariant.

No counter-argument remains an unresolved architecture blocker.

## Cross-artifact and task audit

Strict deterministic validation reports:

- 4 user stories;
- 105 unique functional requirements;
- 59 unique success criteria;
- 201 sequential tasks, 75 marked parallel;
- 105/105 functional requirements traced;
- all 59 success criteria present in traceability;
- zero unresolved placeholders;
- exactly ten policy names in the normative surface;
- Runner adapter and optional observer modeled as independent SPIs.

The distributed-consistency FR-071–FR-082 and SC-033–SC-040 map to the
blocking T051–T064 characterization, implementation and MiniNDN fault gate.
The operational-workflow FR-083–FR-096 and SC-041–SC-050 map to T130–T144 and
the final gates. Former T130–T186 tasks were mechanically shifted to T145–T201
to keep IDs unique and sequential; no task was marked complete.
The iTiger-handoff FR-097–FR-105 and SC-051–SC-059 map to Spec 111 T171/T186/
T200 plus the concrete appended Spec 110 T219–T232 bridge. These requirements
add no container-build/runtime or live-submission authority to Spec 111.

Current-base verification passed 10/10 focused tests: 3 generic bound C++ lease
table tests through Python and 7 `DistributedLeaseTransaction` tests. This proves
the reusable prepare/commit/idempotency/cleanup baseline only; it does not count
as certificate, requester-crash, deployment-fencing or periodic-sweep evidence.

## Readiness scorecard

| Dimension | Ready? | Notes |
| --- | --- | --- |
| Intent and scope | Yes | External Python team can express joint, scoped, stateful optimization without Core edits. |
| Architecture and ownership | Yes | Ten policies and two SPIs have distinct authority; requester/deployment coordination reuses Provider-local lease authority. |
| Security/correctness | Yes | Exact constraints, least input, tenancy, certificates, three fencing epochs and bounded orphan cleanup are normative. |
| Task executability | Yes | Concrete files, ordered T051–T064 failure gates and evidence paths exist; Engine work depends on T064. |
| Validation/evidence | Yes for design only | Structure and semantic audit pass; implementation/local package/static and MiniNDN evidence is future work; container runtime is deferred. |
| Migration/rollback | Yes | Parity defaults, compatibility and lifecycle/intent rollback gates remain bounded. |
| Deployment/use workflow | Yes for design only | Immutable revisions, durable operation/request handles, readiness, bootstrap, upgrade/rollback, drain/shutdown and CLI parity are specified; T130-T144 remain unimplemented. |
| iTiger batch handoff | Yes for design only | OCI-to-SIF, Slurm/APP/request ownership, binds, topology v2, network/preemption and new-candidate gates are explicit; Spec 110 T219-T232 remain unimplemented. |
| Code reality | Yes | Proposed versus current/executed/measured status is explicit. |

## Evidence limits

1. No SDK implementation, wheel, native target, MiniNDN run, container/SIF,
   Qwen inference or iTiger job was executed in this document revision.
2. Existing lease regression tests characterize a useful base, but the new
   commit certificate, periodic cleanup, deployment fencing and requester-crash
   matrix remain proposed until T051–T064 execute.
3. The decision-point inventory remains T084 work; until it passes, runtime
   closure is proposed rather than proven.
4. Model quality facts are declared evidence inputs, not a measured quality
   guarantee from Core.
5. GSD health is degraded only by the unrelated stale Spec 110 worktree
   `/tmp/spec110-local-gpu-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0`;
   this revision did not modify or remove it.
6. No operational readiness claim is permitted until T144 records the complete
   clean-profile MiniNDN workflow; package/image success alone is insufficient.
7. No post-Spec-111 SIF, GPU or Qwen job was executed. Current Spec 110
   materialization/GPU gates remain open and its published pre-Spec-111 image is
   not deployment evidence for this feature.
8. Spec 111 completion must contain zero new OCI/SIF build or runtime evidence;
   that absence is intentional scope compliance, not a missing Spec 111 test.

## Decision and next action

Spec 111 passes the revised pre-implementation design gate. Implementation must
still start with inventory/characterization T001–T030. Large-scale SDK/default
migration remains blocked until T030 passes, T064 proves distributed
consistency, and T084 proves decision-point closure. Operational-readiness claims
also remain blocked until T130–T144 implement and prove the deployment/use
workflow. The next highest-value step is therefore T001–T012 setup/inventory,
then behavior characterization and the T051–T064 consistency gate; after the
Core/APP interfaces stabilize, execute T130–T144 before broad packaging or
remote inference. Then close the offline handoff through T171/T186/T200 and
Spec 110 T219–T228; only a new Spec 110 candidate may proceed to T229–T232—not
immediate implementation of all ten ports or direct Docker execution on iTiger.

This audit does not authorize iTiger execution. Any remote run requires a new
candidate identity, applicable offline gates and explicit submission authority.

## 2026-07-15 Post-Implementation Durability and Performance Re-Audit

**Mode**: strict code-aware post-implementation  
**Structural scan**: PASS with non-contiguous-ID warning; 105 FRs, 59 SCs,
4 user stories, 217 tasks before remediation, 211 checked  
**Verdict**: **BLOCK**  
**Findings**: 0 CRITICAL, 3 HIGH, 1 MEDIUM, 0 LOW

The implementation cannot be declared complete. Two clean candidates preserve
correctness but fail SC-007, and code inspection shows that checked T137/T210
do not yet implement the full FR-086/FR-090 durability contract. No container,
iTiger or Slurm execution is authorized by this audit.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
| --- | --- | --- | --- | --- | --- |
| P1 | HIGH | Code reality / durability | `spec.md:793-805`, `tasks.md:T137`, `app_sdk/runtime_journal.py:19-230`, `test_ndnsf_di_runtime_journal.py` | T137 is checked, but the journal has no bounded compaction, explicit unsupported-version rejection, typed lock-contention/unsafe-root mode or complete retention behavior required by FR-086 and SC-045. The 2026-07-15 repair added real protected-envelope file and directory sync, but that closes only one durability defect. | Complete T218 before another candidate. Inject every SC-045 failure and prove restart preservation; never fall back to volatile authority. |
| P2 | HIGH | API / distributed recovery | `spec.md:825-832`, `contracts.py:102-109`, `client.py:438-458` | The public six-field `RequestHandle` binds only request/deployment/revision/timestamps/envelope digest. It has no requester identity, attempt epoch, certificate, security/locator reference or result rendezvous. `open_request()` is a dictionary lookup and cannot perform the typed same-identity retry/recovery required by FR-090/SC-044. | Complete T219 with the canonical `InferenceRequestHandle`/`RequestEnvelopeReference`, real network identity binding, fenced cancellation and restart tests. |
| P3 | HIGH | Validation / performance | `spec.md:1046-1051`, `plan.md:695-704`, T212/T215 evidence | Both admissible 20-cell candidates fail SC-007. T215 p50/p95 bootstrap upper bounds are `+11.3445%/+7.8100%`. Native-only p50 still reaches `+8.0838%`, while the corrected durable APP diagnostic adds p50 `5.832 ms`. Compatibility deletion and completion remain blocked. | Keep SC-007 unchanged, preserve both negatives, close P1/P2, optimize without weakening durability, and allow only the prospective T220 candidate/matrix. |
| P4 | MEDIUM | Evidence integrity | `plan.md:688-693`, formal harness state roots, `evidence/t215-negative-result-and-durability-audit.md` | The operational workflow requires an operator-configured persistent root, while MiniNDN diagnostics intentionally use and delete isolated `/tmp/spec111-app-state-*` roots. That proves process restart behavior, not host-reboot or production-volume durability. | Label MiniNDN evidence as process-restart durability only; T218 must test unsafe/volatile configuration fail-closed, and Spec 110 must validate the mounted persistent state root later. |

### Traceability gaps

| Source | Missing link | Impact |
| --- | --- | --- |
| FR-086 / SC-045 / T137 | Full journal mechanism and injected evidence are absent despite a checked task. | Crash recovery and authority persistence are not ready. |
| FR-090 / SC-044 / T210 | Current handle does not carry the normative attempt/certificate/rendezvous identity. | Process restart cannot recover the specified distributed attempt. |
| SC-007 / T213 | No accepted performance candidate exists. | Final PASS and compatibility deletion are prohibited. |

### Readiness scorecard

| Dimension | Ready? | Notes |
| --- | --- | --- |
| Intent and scope | Yes | Core/APP separation and MiniNDN-only boundary remain intact. |
| Architecture and ownership | Conditional | APP owns the fixed journal and handles, but their implementation is incomplete. |
| Security/correctness | No | P1/P2 are fail-closed recovery gaps. |
| Task executability | No | Checked T137/T210 overstate code reality; T218/T219 now make remaining work explicit. |
| Validation/evidence | No | Negative matrices are valid evidence; no accepted candidate exists. |
| Migration/rollback | Conditional | Compatibility rollback is correctly blocked. |
| Code reality | No | Durable journal and handle fields fall short of their contracts. |

### Decision

SC-007 remains the controlling end-to-end non-regression gate. Although its
baseline lacks the newly required durable APP mechanism, revising it after two
observed failures into a network-only or looser gate would suppress measured
feature cost. Native timing decomposition may remain diagnostic only. T218 and
T219 must close correctness first; T220 may then optimize the correct mechanism,
run one treatment diagnostic, and create no more than one new formal candidate.
T213 is the final strict PASS gate.

## Phase 2 Pre-Implementation Gate Revalidation

**Date**: 2026-07-14  
**Mode**: code-aware pre-implementation after T001-T029  
**Verdict**: **PASS**  
**Findings**: 0 CRITICAL, 0 HIGH, 0 MEDIUM, 0 LOW

The strict scanner reports 105 FRs, 59 SCs, four user stories, 201 unique
sequential tasks and 105/105 traced FRs. Deterministic analysis found zero
placeholders, zero missing FR/SC traceability links and zero remaining
five-pair/ten-pair contradictions. The ten-pair campaign language now agrees
across spec, plan, tasks and quickstart.

CodeGraph was synchronized and is current at 2,351 files, 50,873 nodes and
168,445 edges. Code reality remains the intended pre-movement state:
`runtime_v1.py` still owns runtime scoring, `deployment.py` still translates
deployment preferences through a process environment variable, `APPDeployment`
is still a definition-only façade, and the package root still exposes the
inventoried legacy surface. These are migration targets, not falsely claimed
completed behavior. The current lease/attempt primitives are reused by the
design; no second authority or leaderless/global-atomicity claim was found.

Executed characterization passed 27 Python tests plus one expected Phase-3
skip, all 264 C++ tests and all six existing security leaves plus their
aggregate gate. The once-only pre-separation MiniNDN canary failed before its
measurement window because the harness cleanup killed the supervising shell;
the negative outcome is retained without rerun. This does not invalidate the
Core/APP ownership design or characterization suite and therefore is an
evidence limitation rather than an unresolved architecture finding. It does
block treating that cell as a measured performance baseline. The immutable
pre-movement source remains the baseline candidate for T187, and supervision
must be corrected before any final campaign cell begins.

GSD health is degraded only by the previously recorded unrelated stale Spec
110 worktree; it has no Spec 111 source ownership or evidence impact. The
implementation and audit were performed directly in this workspace.

The Phase 2 gate authorizes Core extraction T031-T050. It does **not** authorize
large-scale policy SDK/default implementation: FR-055 still blocks that work
until the distributed consistency gate T064 and decision closure T084 also
pass. It does not authorize a container build, iTiger connection or Slurm job.

## 2026-07-15 Diagnostic-Convergence Re-Audit

**Mode**: strict code-aware post-implementation, non-final  
**Structural scan**: PASS with non-contiguous-ID warning; 105 FRs, 59 SCs,
4 user stories and 225 tasks after remediation  
**Verdict**: **BLOCK**  
**Findings**: 0 CRITICAL, 3 HIGH, 1 MEDIUM, 0 LOW

This audit followed the corrected execution rule: repair the current failure,
rerun only its focused gate, continue to the next unstarted unit, and reserve a
clean whole matrix for one stable candidate. It did not restart either T212 or
T215. RuntimeJournal and durable request recovery gaps P1/P2 from the prior
audit are materially closed, but code tracing exposed two additional real-path
gaps before final performance acceptance.

### Findings

| ID | Severity | Dimension | Location | Finding | Required action |
| --- | --- | --- | --- | --- | --- |
| P5 | HIGH | Distributed correctness | `spec.md:833-837`, `app_sdk/client.py:606-637`, `core/recovery.py:97-114`, `NativeProviderHandler.cpp:74-114,812-829` | `APPClient.cancel()` cancels only a process-local Future and journals local `CANCELLED`. Provider code and Core recovery already define attempt-fenced `CANCEL/SUPERSEDE` payloads, but the canonical APP handle does not retain the execution certificate/member set and no APP transport sends the control to every certified Provider. A local terminal state is therefore falsely stronger than observed distributed cancellation. | Complete T222-T223: bind certificate/membership, send authenticated existing-service controls, retain per-Provider evidence, reject stale/replayed/wrong membership and execute one focused MiniNDN fault gate. |
| P6 | HIGH | Validation / performance | `spec.md:1046-1051`, T212/T215 evidence, `evidence/t220-durable-performance-convergence.md` | No accepted SC-007 candidate exists. The correctness-preserving T220 microbenchmark passed, but its only 70-request diagnostic predicts p50/p95 `+12.1083%/+6.5596%` versus readiness baseline, so it correctly spent zero new candidate/matrix rather than knowingly repeating a likely rejection. | Keep the 5% gate and old negatives unchanged. T225 must first predict both metrics within 5%, then may create exactly one new candidate and at most one clean matrix. |
| P7 | HIGH | Persistence / secret ownership | `spec.md:788-805`, `app_sdk/runtime_journal.py:51-108`, `app_sdk/client.py:78-87`, MiniNDN `/tmp/spec111-app-state-*` evidence | The implementation durably syncs an internally generated envelope key, but production client fallback still uses `/tmp` and the key is generated inside the RuntimeJournal-owned state tree. That proves MiniNDN process restart, not the operator-mounted persistent-root and external owner-security boundary; volatile authority is not rejected. | Complete T224 with an injected owner key/secret provider, explicit persistent-root policy, named MiniNDN-only override and missing/wrong/rotation/restart tests. Spec 110 later validates the mounted volume. |
| P8 | MEDIUM | Cross-artifact consistency | `traceability.md`, `tasks.md:T208-T225` | Traceability had stopped at 207 tasks and described T220 as a future candidate after T220 had already chosen zero candidates. | Repaired in this revision: mappings now cover T208-T225, the two immutable negatives, zero-candidate stop and the explicit T222-T225 dependency chain. |

### Closed prior findings and focused evidence

- Prior P1: T218 now has typed outer/nested version, lock, unsafe-root, quota,
  compaction, key-length, durability and corruption gates; 13/13 focused tests
  pass. T224 still owns production key/root policy rather than reopening the
  journal mechanism tests.
- Prior P2: T219 now exposes `InferenceRequestHandle` and
  `RequestEnvelopeReference`, fences requester/attempt identity, validates
  result rendezvous and passed a real Controller/three-Provider/User restart
  workflow. The first failed identity-mismatch attempt remains preserved and
  its one affected rerun passed.
- Dynamic-plan request identity: CodeGraph found the internal facade rejected
  canonical `request_id`; T221 fixed and tested it with 9/9 facade and 14/14
  request-handle tests. No MiniNDN rerun was used for this caller repair.
- T220: 300/300 local requests passed the fixed p50/p95/drift bounds; its one
  70-request MiniNDN treatment diagnostic passed correctness/cleanup and was
  not promoted to a candidate.

### Readiness scorecard

| Dimension | Ready? | Notes |
| --- | --- | --- |
| Intent and scope | Yes | Core/APP split and MiniNDN-only Spec 111 scope remain intact. |
| Architecture and ownership | Conditional | Main ownership is correct; external key ownership and Provider cancellation binding remain open. |
| Security/correctness | No | P5 and P7 can make local state stronger than distributed/persistent evidence. |
| Task executability | Yes | T222-T225 name concrete paths, failure gates and no-rerun boundaries. |
| Validation/evidence | No | Focused gates are valid; SC-007 has no accepted candidate. |
| Migration/rollback | Conditional | Compatibility/final handoff correctly remain blocked. |
| Code reality | No | Dynamic ID is fixed; distributed cancel and production persistence policy are not yet implemented. |

### Decision

Do not run T213, finalize the Spec 110 handoff, play the completion bell or
start another performance matrix. Execute T222-T224 as focused correctness
work. T225 may run a local microbenchmark and one 70-request diagnostic only
after those pass; it may spend a candidate/matrix only if that diagnostic meets
its predeclared prediction gate. Every failed or completed cell remains
terminal and is never rerun merely to advance the workflow.
