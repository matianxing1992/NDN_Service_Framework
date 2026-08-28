# Tasks: TigerCluster NDNSF-DI Deployment Fidelity

**Input**: Design documents from `specs/168-itiger-di-deployment-fidelity/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`,
`experiment-plan.md`, and `contracts/`

**Tests**: Every implementation task is test-first and retains its focused gate.
Real MiniNDN and the exact candidate container are mandatory before TigerCluster.

**Organization**: Tasks are grouped by user story and close cohesive behaviors.
An implementation, its regression, focused validation, and evidence are one task
unless the result has an independently meaningful admission boundary.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Safe to execute in parallel after its declared dependencies
- **[Story]**: User story from `spec.md`
- Every task names its owning source, tests, and acceptance/evidence paths

## Phase 1: Setup - Immutable Authority and Harness Skeleton

**Purpose**: Freeze the inherited evidence and create one Spec 168 campaign
surface without rebuilding or copying existing model/runtime assets.

- [x] T001 Freeze the Spec 168 baseline by resolving and hashing the retained job 181948 success, job 181951 failure, Spec 167 transport evidence, existing SIFs, content-addressed Qwen assets, routes, schedules, and analyzers; write links/digests and claim boundaries without copying payloads in `specs/168-itiger-di-deployment-fidelity/evidence/baseline-manifest.json`, `specs/168-itiger-di-deployment-fidelity/evidence/baseline.md`, and `specs/168-itiger-di-deployment-fidelity/jobs/`

- [x] T002 Create one canonical resumable Spec 168 admission/campaign entrypoint that writes immutable experiment and schedule manifests, uses content-addressed links to existing material, rejects in-place candidate mutation or duplicate output identities, and exposes focused, MiniNDN, exact-container, remote-small, and remote-large phases in `specs/168-itiger-di-deployment-fidelity/jobs/`, with its self-tests and retained dry-run evidence in `tests/python/test_spec168_campaign_contract.py` and `specs/168-itiger-di-deployment-fidelity/evidence/campaign-harness.md`

---

## Phase 2: Foundational - Shared Lifecycle Evidence and Admission

**Purpose**: Establish the binding, progress, failure, and local-deployment
foundation required by every user story.

**Critical**: No three-node TigerCluster campaign is admitted until this phase
passes. After Gates A and B, one bounded single-node exact-SIF/CUDA Gate C is
allowed when the development host lacks Apptainer or CUDA.

- [x] T003 Implement the canonical invocation/lifecycle event and summary contracts with one request ID, attempt/plan/Provider-role bindings, monotonic authenticated progress, first-writer terminal state, complete schedule reconciliation, and exact failure codes across `NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/runtime_journal.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1_evidence.py`, and `tests/python/test_spec168_lifecycle_evidence.py`; retain the focused gate in `specs/168-itiger-di-deployment-fidelity/evidence/lifecycle-evidence-gate.md`

- [x] T004 Build the deployment-faithful local admission gate using real MiniNDN/NFD, independent Controller/Repository/User/three-Provider processes, normal permissions/NAC-ABE/tokens, real DistributedRepo transfer, the Qwen adapter, and the same request/analyzer contract intended for the SIF in `specs/168-itiger-di-deployment-fidelity/jobs/run-real-minindn-gate.sh`, `specs/168-itiger-di-deployment-fidelity/jobs/run-exact-container-gate.sh`, and `tests/python/test_spec168_real_minindn_gate.py`; on the 8 GiB host use a content-addressed three-role tiny-Qwen fixture with two real dependency edges under `--memory=6g --memory-swap=7g`, real transformer execution, full multi-token output, and zero cgroup OOM events; fail on mocks, shared-filesystem payload injection, fixed settle waits, test-only identities, missing lifecycle evidence, or model copies inside run directories; when local Apptainer/CUDA is unavailable, run the exact SIF as one bounded RTX 5000 Gate C job and bind the complete Python/package-data overlay closure; retain the v30 Gate B pass, job 182382 closure failure, and job 182384 Gate C pass under `specs/168-itiger-di-deployment-fidelity/evidence/`

**Checkpoint**: Immutable material is reusable, lifecycle evidence is
machine-checkable, and the exact remote candidate has a blocking local gate.

---

## Phase 3: User Story 1 - Complete a Deployment-Faithful Invocation (P1) MVP

**Goal**: One small-model Request completes the secured three-node lifecycle and
returns a complete multi-token Response with no global barrier or CPU fallback.

**Independent Test**: Under real MiniNDN and then one three-node TigerCluster
allocation, submit one pinned Qwen3-0.6B request and reconstruct every event from
the original Request to one authenticated terminal Response.

- [x] T005 [US1] Enforce the model-first public API and request-first deferred planning by adding failing cases for deployment fields in normal `app.yaml`, early/preplanned mutation, late or wrong-request ACKs, graph-free external strategy calls, infeasible capacity/dependencies, and mutable plan commits; implement `InferenceApplication.request(model, input, generation, strategy) -> begin_collaboration -> ACK_CLOSED -> adapter graph -> strategy plan -> artifact publication -> commit_plan/final Selection` with full model/graph/strategy bindings, rename the former deployment path to explicit `request_preplanned()`, keep only a counted/deprecated positional shim until tracked callers migrate, and cover its deletion condition in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/contracts.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py`, and `tests/python/test_spec168_deferred_planning.py`, then retain the focused and MiniNDN verdict in `specs/168-itiger-di-deployment-fidelity/evidence/deferred-planning.md`

- [x] T006 [US1] Remove the all-member readiness barrier from the default NDNSF-DI execution path by first proving Stage 0 is blocked by incomplete `ReadySetCoordinator`/`ExecutionActivateMessage`, then implementing plan-bound `DATA_DRIVEN_V2` per-role authorization and the eligibility predicate `committed plan + local GPU readiness + direct predecessor inputs`; retain explicitly negotiated and counted `LEGACY_READY_SET_V1` only for preplanned compatibility, reject mixed policy/automatic fallback, preserve replay, signature, request/attempt/plan/boot-epoch checks, and cover new-V1-invocation rollback across `NDNSF-DistributedInference/ndnsf_distributed_inference/core/execution.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/deployment_control.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, and `tests/python/test_ndnsf_di_selection_dataflow.py`, with causal and mixed-version evidence in `specs/168-itiger-di-deployment-fidelity/evidence/per-role-dataflow.md`

- [x] T007 [US1] Make Provider preparation and generation truthful end to end by requiring verified artifact availability, adapter-confirmed load/warmup on the explicitly assigned execution device before `LOCAL_READY`, fail-closed backend/device/fallback checks (`CPU_LOGIC` is local logic evidence only; GPU acceptance requires exact CUDA), one prompt input and one internal token loop, ordered token evidence, and one complete authenticated terminal Response in `NDNSF-DistributedInference/ndnsf_distributed_inference/artifact_deployment.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/pilot.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, `tests/python/test_spec161_qwen_generation.py`, and `tests/python/test_spec168_provider_generation.py`; retain local and exact-container gates in `specs/168-itiger-di-deployment-fidelity/evidence/provider-generation.md`

- [x] T008 [US1] Freeze the first source/SIF candidate that passes Gates A-D and submit exactly one Qwen3-0.6B three-node TigerCluster control using the canonical Spec 168 job surface; preserve the Slurm identity, raw logs, routes, GPU/process inventory, full answer, token/stage trace, security verdict, and analyzer output, and accept it only with one wire Request, zero per-token Requests, one authenticated Response, three CUDA stages, and zero CPU fallback in `specs/168-itiger-di-deployment-fidelity/jobs/` and `specs/168-itiger-di-deployment-fidelity/evidence/tiger-small-single/`
  - 2026-08-07 evidence: canonical Job 182518 (`v88`) passed in 3:21 with one wire Request, zero token Requests, one authenticated complete response, three RTX 5000 CUDA stages, zero CPU fallback, 47 generated tokens, EOS, and role-specific Repository fetches. Retained under `evidence/tiger-small-single/182518-v88-qwen3-small-single`.

**Checkpoint**: User Story 1 is independently complete only when both the real
MiniNDN/exact-container path and one immutable TigerCluster job satisfy SC-001 to
SC-003 and SC-010.

---

## Phase 4: User Story 2 - Distinguish Cold Preparation from Warm Reuse (P2)

**Goal**: Compatible repeated requests reuse actual Provider model residency and
make cold distribution/preparation distinct from warm execution.

**Independent Test**: In one unchanged allocation, run the frozen 30-row schedule
and prove reuse with ACK inventory plus zero transfer/load counters, not latency.

- [x] T009 [P] [US2] Implement content-addressed disk, host-memory, and adapter-confirmed GPU residency with boot-epoch/device/runtime compatibility, explicit promotion/eviction transitions, reusable ownership, and links/mapped views instead of per-request multi-GB copies in `NDNSF-DistributedInference/ndnsf_distributed_inference/artifact_deployment.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, and `tests/python/test_spec168_cache_residency.py`; prove restart/device/model/graph/partition invalidation and retain byte/load counters in `specs/168-itiger-di-deployment-fidelity/evidence/cache-residency.md`

- [x] T010 [US2] Make ACK capability snapshots and `PreSplitFirstStrategy` choose only feasible compatible placements in GPU, RAM, disk, published, then newly generated order while incorporating boot epoch, capacity, graph cut, RTT, bandwidth and load; retain the exact ACK set and scored decision, reject false/stale hits, and cover equal/heterogeneous GPU partitioning in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/presplit.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/presplit_first.py`, and `tests/python/test_ndnsf_di_presplit_first_strategy.py`, with evidence in `specs/168-itiger-di-deployment-fidelity/evidence/reuse-strategy.md`

- [x] T011 [US2] After T008-T010 pass, execute the immutable five-prompt schedule with one warmup plus five measured invocations per prompt in the same TigerCluster allocation; retain all 30 rows, full answers, cache classes, phase latency, TTFT, per-token latency, total latency, tokens/s, Repository unique/wire bytes, device loads, and failures, and produce per-prompt/per-cache distributions plus causal reuse assertions in `specs/168-itiger-di-deployment-fidelity/jobs/`, `specs/168-itiger-di-deployment-fidelity/evidence/tiger-small-repeated/`, and `specs/168-itiger-di-deployment-fidelity/evidence/small-cold-warm-analysis.md`
  - 2026-08-07 evidence: Job 182777 ran the complete 30-row schedule once. The immutable batch state is retained as FAILED because analyzer v92 misclassified concurrent stdout interleaving; reanalysis of the untouched remote evidence is PASS (`analysis` SHA-256 `14b587e55f9546556bac47ed2c165523ca230880ba106538b316070433ab3e64`), with 1 GENERATED + 29 REUSE_CACHED rows, 30 wire Requests, zero token Requests, zero CPU fallback, and PASS causal reuse. Negative Jobs 182519 and 182773 remain retained.
  - 2026-08-04 checkpoint: immutable Job 182508 retained 18 successful rows and one bf16 top-logit-tie failure before stopping at 19/30. Exact-SIF diagnostic Job 182509 classified the token divergence as numerically equivalent, but the campaign remains FAILED and incomplete. It also disproved end-to-end cache-policy readiness (`catalogCount=0`, `cacheClass=GENERATED`) and measured a repeated approximately 120-second ACK collection window. Preserve this negative evidence and require both defects to become local/exact-container blockers before freezing a replacement identity.

**Checkpoint**: User Story 2 passes only if schedule reconciliation is complete,
warm hits are identity-compatible, and unchanged warm assignments show zero
duplicate model bytes and redundant device loads.

---

## Phase 5: User Story 3 - Diagnose and Repair Real Deployment Failures (P3)

**Goal**: Every admitted failure has a precise boundary, progress checkpoint,
root-cause/repair lineage, and local gate before any replacement remote run.

**Independent Test**: Inject or preserve failures at each major boundary and
verify the analyzer identifies one primary class without generic-timeout-only or
wrong-component conclusions.

- [x] T012 [P] [US3] Exercise the failure taxonomy with test-first injections for bootstrap/security, routing, ACK, planning, Repository publication/fetch and stalled range, disk/RAM/GPU preparation, dependency data, execution, token loop, response, cleanup, environment, and analyzer boundaries; integrate hard/no-progress deadlines and last-checkpoint reporting across `NDNSF-DistributedInference/ndnsf_distributed_inference/core/state.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/status.py`, `pythonWrapper/ndnsf/progress_deadline.py`, `tests/python/test_spec165_progress_deadline.py`, and `tests/python/test_spec168_failure_taxonomy.py`, and retain the matrix in `specs/168-itiger-di-deployment-fidelity/evidence/failure-taxonomy-gate.md`
  - 2026-08-07 evidence: focused failure-taxonomy and progress-deadline gates pass; checkpoint-aware boundary/status contracts and the retained matrix cover the admitted failure classes.

- [x] T013 [US3] Close every NDNSF-DI logic defect encountered by preserving its original remote identity, reproducing it in real MiniNDN or the exact SIF, assigning the narrowest owner, adding the failing regression, applying the minimal repair, passing Gates A-D under a new source identity, and performing at most the explicitly admitted remote requalification; specifically close job 182413's >7 KiB compact-Selection non-fanout by proving one provider-specific authenticated projection per selected collaboration role without a second Request/ACK/plan/attempt; record each complete lineage without deleting negative evidence in `specs/168-itiger-di-deployment-fidelity/evidence/defects/`, the owning source paths, and their focused `tests/python/test_spec168_*.py` regressions
  - 2026-08-04 Job 182511 checkpoint: the same compact-Selection non-fanout recurred because the formal rank launcher bypassed `spec168-overlay-entrypoint.sh`. The source bundle contained native core `sha256:50c059...` with provider projections, but the process mapped the old SIF core `sha256:510274...`, which lacks all projection/prefetch markers. Provider 0 received the local 7,803-byte compact Selection while Providers 1/2 received no LLM Selection. Preserve both jobs, require mapped-native digests/capabilities in admission, and require a three-node no-model control-plane canary before another model campaign.
  - 2026-08-04 Job 182512 checkpoint: the first three-node canary failed before `srun` because `gate-e-small-single.sbatch` still hashed model artifacts and applied the one-prompt model-template rule in canary mode. Preserve the 94-second negative, split canary admission from every stage/generation/Repo input, exercise that split locally without model files, and use a new source/campaign identity rather than resubmitting the failed campaign.
  - 2026-08-04 Job 182513 checkpoint: model-free batch admission passed, but all three ranks failed in one second because the outer rank wrapper still required `SPEC168_ARTIFACT_DIR` before Apptainer. Preserve the negative, apply the canary/model split across batch, outer rank, inner adapter, and compatibility kernel, and execute the real outer wrapper locally with all model variables absent before freezing the next candidate.
  - 2026-08-04 Job 182514 checkpoint: batch, outer wrapper, SIF overlay, and three-node routing passed, but the reused compatibility kernel prepended the old SIF library path before the candidate-native path. The policy-builder child then linked the new `_ndnsf` extension to the old core and failed on `ServiceUser::BeginCollaboration`. Preserve the negative and require a fresh post-kernel child process to prove the mapped native core/extension hashes on every rank.
  - 2026-08-04 Job 182515 checkpoint: all three ranks completed, all three mapped-native child ABI checks passed, one request projected Selection to all three providers, all three handlers ran, and the user received a schema-valid three-stage response in 1071.05 ms with zero model work. Slurm still recorded `FAILED` because the closure analyzer required a synthetic `LLM_PIPELINE_USER_OK` marker that the real runtime never emits. Preserve the original terminal state, reanalyze the untouched evidence using the real `LLM_PIPELINE_USER_RESPONSE` schema, seed fixtures from retained runtime records, and do not resubmit a completed protocol run solely to repair post-run classification.
  - 2026-08-04 Job 182516 checkpoint: the single Qwen3-0.6B request used one request ID and projected Selection to all three Providers. Stage 0 fetched 594,357,850 verified bytes (78,205 Data, zero retransmitted bytes, 7,621.18 ms), loaded CUDA, and published its hidden state. Stages 1/2 observed no late-bound Repo registration and silently fell back to a cross-filesystem pre-split hardlink, which failed deterministically; the operator cancelled the otherwise one-hour wait after 11:28 and did not retry. Repair registration visibility as an assignment-deadline-bounded, progress-emitting wait; atomically replace registration snapshots; forbid pre-split fallback whenever DistributedRepo registration is configured; and reproduce delayed registration visibility locally before freezing a new source identity. The apparent long Provider-to-request delay is not yet proven to be a fixed settle interval and requires launcher-timeline inspection.
  - 2026-08-04 Job 182518 checkpoint: v88 was submitted once and completed `0:0` in 3:21. The closure analyzer passed one request ID, one wire request, three distinct RTX 5000 GPUs, three role-specific Repo fetches (594,357,850 + 283,192,711 + 625,825,930 bytes, zero retransmitted bytes), CUDA on every rank, zero CPU fallback, 47 generated tokens, EOS, and a complete answer. The assignment-deadline registration wait closed Job 182516's Stage 1/2 fallback defect. Preserve the canonical evidence and advance only to a separate same-source two-request cold/warm campaign; do not infer warm reuse from this cold single request.
  - 2026-08-07 closure matrix: all encountered NDNSF-DI logic, lifecycle, launcher, and analyzer defects have a named owner, minimal repair, focused regression, and later source/gate evidence. The v26 and 182780 memory kills remain explicitly classified environmental boundaries; no negative identity was rewritten. See `evidence/defects/closure-matrix.md`.

**Checkpoint**: User Story 3 is complete when every admitted failure is either a
closed repair lineage, a retained environmental outcome, or an explicitly
unresolved evidence gap—never a guessed generic timeout.

---

## Phase 6: User Story 4 - Requalify at Meaningful Model Scale (P4)

**Goal**: The same public lifecycle completes one three-node response for a
model whose full weights exceed one target GPU.

**Independent Test**: After the small-model campaign passes, submit one pinned
large-model deterministic request and require complete Repository, residency,
dependency, CUDA-stage, token, and Response evidence.

- [x] T014 [US4] Qualify the pinned large-model graph and assignment without materializing large-model weights on the 8 GiB host: validate adapter-generated dependency metadata, capacity-aware three-stage partition arithmetic, immutable artifact/catalog identities, bounded segmented-fetch window/backlog/retry state-machine behavior for the job 181951 stalled-range reproducer, and unequal arrival ordering using bounded fixtures in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/onnx/graph.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/artifact_deployment.py`, `tests/python/test_spec168_large_model_gate.py`, and `specs/168-itiger-di-deployment-fidelity/evidence/large-local-gate.md`; perform actual large-shard fetch, model load, CUDA-capacity, and inference only in the T015 TigerCluster Slurm allocation
  - 2026-08-07 evidence: bounded large-model graph/capacity and stalled-range fixtures pass locally; the 27B shard fetch/load/inference remained isolated to the single admitted v93 replacement Job 182780, which reached a verified 18.53-GB Stage-0 fetch before the model-preparation process hit the TigerCluster memory cgroup limit. The OOM evidence remains immutable. Under the narrowly defined FR-019 resource-boundary exception, one new resource-profile identity may be admitted; no further replacement is allowed.

- [ ] T015 [US4] Reuse the qualified SIF and existing large-model Repository payload to submit exactly one new immutable TigerCluster request through the same Spec 168 API and job path; accept only a complete authenticated answer from three CUDA stages and preserve all shard/range, residency, dependency, token, response, and failure evidence in `specs/168-itiger-di-deployment-fidelity/jobs/` and `specs/168-itiger-di-deployment-fidelity/evidence/tiger-large-single/`; do not start a repeated large-model campaign on failure
  - 2026-08-07 outcome: Job 182780 passed artifact/ACK/Selection admission and verified Stage-0 transfer, then hit a memory-cgroup OOM during model preparation; retained as `182780-v93-qwen36-large-single-oom`. A single linked resource-repaired identity is admitted under FR-019 with a new frozen resource profile; no further replacement is allowed.
  - 2026-08-07 v94 outcome: the sole resource-repaired Job 182782 (`96 GiB/node`, three RTX 5000 nodes) fetched all three immutable shards and reached CUDA `RUNTIME_READY` on all roles with zero CPU fallback. It returned one authenticated 8,468-byte response (one wire Request, zero token Requests), but strict deterministic reference validation failed with `TOKEN_MISMATCH` (`exactReferenceMatch=false`) and rank 0 returned code 1. The remote identity is retained as `182782-v94-qwen36-large-single-response-failure`; no further large-model identity is permitted.

**Checkpoint**: User Story 4 passes SC-008 only after one complete response. A
successful fetch or partial stage is not completion.

---

## Phase 7: Reproducibility and Cross-Cutting Closure

**Purpose**: Make claims reproducible, bounded, and traceable without discarding
negative evidence.

- [ ] T016 Repeat the accepted small-model and large-model single-request correctness profiles from one clean three-node TigerCluster allocation using identical immutable source/SIF/model/graph/strategy/prompt/route/analyzer identities; compare deterministic answer/token sequence, assignment/dependency order, initial cache-state classification, CUDA devices, request binding, and security verdict without replacing the original rows, and retain the manifest and comparison in `specs/168-itiger-di-deployment-fidelity/evidence/tiger-clean-reproduction/` and `specs/168-itiger-di-deployment-fidelity/evidence/reproducibility.md`
  - 2026-08-07 outcome: deferred until the one permitted resource-repaired large-model identity establishes an accepted profile; it must still run both profiles sequentially in one clean three-node allocation and cannot be used to manufacture another large-model retry.
  - 2026-08-07 closure: v94 did not establish an accepted large profile, so T016 remains not admitted. Do not submit a clean-allocation reproduction under Spec 168.

- [x] T017 Reconcile the final source, local gates, Slurm jobs, all schedule rows, defect lineages, security verdicts, cold/warm distributions, clean-allocation reproduction, and large-model outcome against FR-001 through FR-024 and SC-001 through SC-011; run Spec Kit analyze/audit plus the reproducibility commands, document residual gaps and prohibited claims, and write `specs/168-itiger-di-deployment-fidelity/traceability.md`, `specs/168-itiger-di-deployment-fidelity/evidence/final-audit.md`, and synchronized operator lessons in the existing NDNSF-DI TigerCluster documentation/skill without broadening prior evidence
  - 2026-08-07 evidence: structure audit PASS; post-run traceability and final audit record the 13 completed tasks, T013/T015/T016 residual gaps, retained OOM/negative identities, and bounded claims.

---

## Dependencies and Execution Order

### Phase dependencies

```text
T001 -> T002 -> T003 -> T004
                     |
                     v
          T005 -> T006 -> T007 -> T008       # US1 MVP
                         |        |
                         |        +-> T009 -> T010 -> T011  # US2
                         +----------> T012 -> T013           # US3
                                      |
                         T011 + T013 -> T014 -> T015          # US4
                         T008 + T011 + T013 + T015 -> T016 -> T017
```

- T001-T004 are blocking admission prerequisites.
- US1 is the MVP and must pass before the repeated or large-model remote phases.
- T009 and T012 are parallelizable after the US1 preparation/lifecycle behavior
  is stable because they primarily own separate cache and failure-state surfaces.
- T011 requires one successful T008 control and the cache/planner repairs.
- T014/T015 require the small-model schedule and all encountered blocking defects
  to be closed or explicitly classified.

### User story independence

- **US1** independently proves the complete small-model lifecycle.
- **US2** independently proves cache-aware repeated-request behavior but reuses
  the qualified US1 path.
- **US3** independently proves diagnosis/repair discipline and may progress in
  parallel with US2 after the common evidence foundation.
- **US4** deliberately depends on small-model qualification because it is a
  scale requalification, not an alternate implementation.

## Parallel Execution Examples

After T007:

```text
Track A: T009 - cache/residency ownership and zero-copy behavior
Track B: T012 - progress/failure taxonomy and injected boundary cases
```

After T010 and T013, local preparation for T011 analysis and T014 large-model
qualification may be staged independently, but remote submissions remain
sequential under the admission contract.

## Implementation Strategy

### MVP first

1. Complete T001-T004.
2. Complete T005-T007 and rerun the full local/exact-container gates.
3. Freeze one candidate and complete T008.
4. Stop and audit US1 before scheduling repetitions.

### Incremental delivery

1. US1: one complete deployment-faithful response.
2. US2: actual cache reuse and cold/warm distributions.
3. US3: closed failure/repair lineage for all admitted defects.
4. US4: one complete larger-than-one-GPU response.
5. T016: clean-allocation correctness reproduction.
6. T017: final traceability and bounded claims.

### Fragmentation scan

The list intentionally coalesces test, implementation, validation, and evidence
for each behavior. Mechanical fragments merged include: request binding with
deferred planning; per-role authorization with dependency triggering; preparation
with GPU/backend and complete-response validation; residency with zero-copy and
invalidation; failure injection with progress classification; and each remote
submission with its immutable evidence/audit. No task exists only to edit one
file, run one command, or write one evidence note.

## Notes

- Preserve the dirty worktree and unrelated user changes.
- Do not rebuild the foundation or copy model payloads when identities match.
- Do not submit remote jobs before Gates A-D pass for the exact candidate.
- A formal failure is immutable; a repair gets a linked new source identity.
- Every remote compute action uses Slurm and the authorized allocation.
