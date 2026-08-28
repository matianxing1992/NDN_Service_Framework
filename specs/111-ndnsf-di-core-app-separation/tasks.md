# Tasks: NDNSF-DI Core/APP Separation

**Input**: Design documents from `specs/111-ndnsf-di-core-app-separation/`

**Prerequisites**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Architecture, compatibility, security, concurrency, behavior, local
package builds, static/offline container-handoff checks and MiniNDN tests are
mandatory because this feature moves authority-adjacent distributed runtime
code. Every distributed network/security/fault/operational/performance gate uses
MiniNDN. Spec 111 never builds OCI/SIF, starts a container runtime or submits an
iTiger/Slurm job; those actions are deferred to Spec 110.

**Organization**: Tasks are grouped by independently testable user story. No
implementation deletion is allowed before its characterization and rollback
tasks pass.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel when prerequisites are satisfied and files do not overlap
- **[Story]**: Maps to `US1`-`US4` in `spec.md`
- Every task names an exact target path or evidence artifact

## Phase 1: Setup, Scope, and Immutable Baseline

**Purpose**: Freeze scope and candidate lineage before source movement.

- [X] T001 Record the pre-separation git/source/dependency identity and dirty-worktree exclusions in `specs/111-ndnsf-di-core-app-separation/evidence/baseline-lineage.md`
- [X] T002 [P] Record historical Spec 107/109/110 evidence paths and file digests without modifying them in `specs/111-ndnsf-di-core-app-separation/evidence/historical-evidence-baseline.json`
- [X] T003 [P] Generate the initial Python import/export/caller inventory in `specs/111-ndnsf-di-core-app-separation/evidence/python-surface-inventory.json`
- [X] T004 [P] Generate the command/config/schema inventory from `NDNSF-DistributedInference/setup.py`, packaging and tools in `specs/111-ndnsf-di-core-app-separation/evidence/command-config-inventory.json`
- [X] T005 [P] Generate the C++ target/header/executable caller inventory from root `wscript`, `examples/wscript`, `tests/wscript`, tests and examples in `specs/111-ndnsf-di-core-app-separation/evidence/cpp-surface-inventory.json`
- [X] T006 [P] Generate the example/experiment/container/systemd/Slurm/Apptainer caller inventory in `specs/111-ndnsf-di-core-app-separation/evidence/deployment-caller-inventory.json`
- [X] T007 Consolidate T003-T006 into the machine-readable initial manifest at `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T008 Validate every initial compatibility entry has one surface kind, current owner, caller list, rollback release and proposed canonical owner in `tests/python/test_ndnsf_di_compatibility_manifest.py`
- [X] T009 [P] Add a historical-evidence immutability fixture referencing T002 in `tests/fixtures/ndnsf-di-core-app-separation/historical-evidence-baseline.json`
- [X] T010 Add a Spec 110 isolation check that rejects edits or evidence promotion into Spec 110 from Spec 111 in `tests/python/test_ndnsf_di_candidate_lineage.py`
- [X] T011 Run CodeGraph caller analysis for `runtime_v1.py`, `app.py`, `deployment.py`, root exports and native runtime and record exact commands/results in `specs/111-ndnsf-di-core-app-separation/evidence/codegraph-baseline.md`
- [X] T012 Commit or otherwise preserve the Phase 1 inventories as the rollback comparison point and record the identity in `specs/111-ndnsf-di-core-app-separation/evidence/baseline-lineage.md`

---

## Phase 2: Foundational Characterization and Architecture Gates

**Purpose**: Build blocking tests before any implementation is moved.

**⚠️ CRITICAL**: No user-story implementation begins until this phase passes.

- [X] T013 Add an allowed/forbidden dependency fixture derived from `contracts/ownership-matrix.md` in `tests/fixtures/ndnsf-di-core-app-separation/ownership-matrix.json`
- [X] T014 Implement an AST/import-graph ownership test with progressive phase gates in `tests/python/test_ndnsf_di_architecture_imports.py`
- [X] T015 Add a Core-only module-load snapshot helper in `tests/python/support/ndnsf_di_import_snapshot.py`
- [X] T016 Add negative optional-dependency import fixtures for ONNX, Qwen, llama, GUI and operations modules in `tests/fixtures/ndnsf-di-core-app-separation/optional-imports.json`
- [X] T017 Add root-export behavior characterization for all supported entries in the compatibility manifest in `tests/python/test_ndnsf_di_legacy_exports.py`
- [X] T018 Add console command/subcommand behavior characterization for `ndnsf-di` and `ndnsf-di-policy` in `tests/python/test_ndnsf_di_legacy_cli.py`
- [X] T019 [P] Add Runtime v1 score golden fixtures for current residency, queue, confidence, network and fallback behavior in `tests/fixtures/ndnsf-di-core-app-separation/runtime-v1-score-golden.json`
- [X] T020 [P] Add deployment preference golden fixtures for active, degraded, inactive and malformed records in `tests/fixtures/ndnsf-di-core-app-separation/deployment-preference-golden.json`
- [X] T021 Extend runtime-aware planner tests to consume T019 without changing expected decisions in `tests/python/test_ndnsf_di_runtime_aware_planner.py`
- [X] T022 Add deployment preference tests consuming T020 to freeze current legacy translation behavior in `tests/python/test_ndnsf_di_deployment_preference.py`
- [X] T023 [P] Add native execution-plan/session/runner behavior references to `tests/unit-tests/distributed-inference-async-runtime.t.cpp`
- [X] T024 [P] Add cache/session/attempt-epoch behavior characterization to `tests/unit-tests/distributed-inference-async-runtime.t.cpp`
- [X] T025 [P] Add Qwen generation-session and stale-attempt characterization to `tests/unit-tests/di-qwen-generation-session.t.cpp`
- [X] T026 Record the exact existing security regression matrix and expected counts in `specs/111-ndnsf-di-core-app-separation/evidence/security-baseline.md`
- [X] T027 Record the frozen MiniNDN command, topology, workload, warmup, logging, timeout, sampler, ten-pair order, seeds, no-rerun rule and 5% non-regression analysis in `specs/111-ndnsf-di-core-app-separation/evidence/performance-baseline-recipe.md`
- [X] T028 Run T008, T010, T014-T025 and the T026 security matrix before movement and record command/counts/durations in `specs/111-ndnsf-di-core-app-separation/evidence/characterization-baseline.md`
- [X] T029 Run the frozen 60-second pre-separation MiniNDN canary exactly once and record admissible or failed measured evidence in `specs/111-ndnsf-di-core-app-separation/evidence/pre-separation-canary.md`
- [X] T030 Declare Phase 2 PASS/BLOCK with characterization results plus strict revised-contract structure, deterministic cross-artifact analysis and code-aware pre-implementation audit evidence; block T031 onward on any unresolved CRITICAL/HIGH finding and record the decision without result replacement in `specs/111-ndnsf-di-core-app-separation/evidence/foundation-gate.md`

**Checkpoint**: Current behavior and rollback baseline are admissible before extraction.

---

## Phase 3: User Story 1 - Stable Distributed-Inference Core (Priority: P1) 🎯 MVP

**Goal**: Extract a workload-neutral, deep DI Core while legacy imports remain thin adapters.

**Independent Test**: Core-only import plus native deterministic plan/session
smoke passes with zero forbidden owner imports; a MiniNDN fault matrix proves
that only a complete authenticated receipt certificate can activate one request
attempt, stale request/deployment/Provider epochs are fenced, and idle orphan
resources are reclaimed within their declared bound.

### Contract and negative tests

- [X] T031 [P] [US1] Add Core contract round-trip and invalid-schema tests in `tests/python/test_ndnsf_di_core_contracts.py`
- [X] T032 [P] [US1] Add Core eligibility stale/infeasible/fragment/lease rejection tests in `tests/python/test_ndnsf_di_core_eligibility.py`
- [X] T033 [P] [US1] Add Core recovery deadline/attempt/exclusion/stale-result tests in `tests/python/test_ndnsf_di_core_recovery.py`
- [X] T034 [P] [US1] Add Core state/cache/session binding tests in `tests/python/test_ndnsf_di_core_state.py`
- [X] T035 [P] [US1] Add Core execution adapter tests against deterministic fake native sessions in `tests/python/test_ndnsf_di_core_execution.py`
- [X] T036 [US1] Make the Core-only dependency gate fail on current forbidden imports in `tests/python/test_ndnsf_di_architecture_imports.py`

### Core extraction

- [X] T037 [P] [US1] Create owner-neutral plan, assignment, evidence and identity contracts in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py`
- [X] T038 [P] [US1] Create immutable candidate normalization and eligibility implementation in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/eligibility.py`
- [X] T039 [P] [US1] Create cache/session/attempt binding implementation by moving current behavior to `NDNSF-DistributedInference/ndnsf_distributed_inference/core/state.py`
- [X] T040 [P] [US1] Create bounded recovery implementation by moving current behavior to `NDNSF-DistributedInference/ndnsf_distributed_inference/core/recovery.py`
- [X] T041 [US1] Create execution orchestration over the existing native seam in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/execution.py`
- [X] T042 [US1] Add the minimal Core public surface with no optional imports in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/__init__.py`
- [X] T043 [US1] Move generic plan/assignment/evidence serialization from `runtime_v1.py` to Core and leave canonical re-exports in `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py`
- [X] T044 [US1] Move eligibility and feasibility implementation from `runtime_v1.py` to Core and leave no duplicate logic in `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py`
- [X] T045 [US1] Move bounded recovery implementation from `deployment.py` and `runtime_v1.py` to Core and leave canonical adapters in their legacy modules
- [X] T046 [US1] Move Core-owned cache/session/attempt binding implementation from `runtime_v1.py` to Core without moving workload policy
- [X] T047 [US1] Update `provider.py` to import Core contracts/mechanisms without importing planner, APP, model or operations owners in `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`
- [X] T048 [US1] Update `client.py` to consume the Core execution surface while preserving existing result/deployment-session behavior in `NDNSF-DistributedInference/ndnsf_distributed_inference/client.py`
- [X] T049 [US1] Update `deployment.py` to consume Core recovery/contracts while leaving application deployment workflow outside Core
- [X] T050 [US1] Bind Python Core execution to existing C++ plan/assignment/session/runner interfaces without changing their ABI in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/native.py`

### Distributed execution and deployment consistency gate

- [X] T051 [P] [US1] Characterize canonical `GenericExecutionLease`, `ProviderExecutionLeaseTable` and `DistributedLeaseTransaction` prepare/commit/abort/release/idempotency/expiry behavior without changing it in `tests/python/test_ndnsf_execution_lease_table.py` and `tests/python/test_ndnsf_di_execution_lease_transaction.py`
- [X] T052 [P] [US1] Add deterministic transport fixtures for dropped, duplicated, reordered and conflicting prepare/commit/abort/release operations and Provider boot-epoch changes in `tests/fixtures/ndnsf-di-core-app-separation/distributed-consistency/`
- [X] T053 [P] [US1] Add contract tests proving exact authenticated receipt-set equality, certificate digest stability and no activation from prepared/partial-commit state in `tests/python/test_ndnsf_di_execution_consistency.py`
- [X] T054 [P] [US1] Add requester crash/restart, same-identity resume, higher-attempt fencing, result-rendezvous and exactly-one-visible-terminal-result tests in `tests/python/test_ndnsf_di_requester_recovery.py`
- [X] T055 [P] [US1] Add deployment owner/lifecycle-epoch compare-and-set, idempotent replay, conflicting-writer and destructive-partial-action tests in `tests/python/test_ndnsf_di_deployment_fencing.py`
- [X] T056 [P] [US1] Add network-partition, Provider-restart, post-cleanup stale-operation replay and no-new-traffic periodic orphan-reclamation tests covering leases, reservations, sessions, cache pins, runner handles and bounded fencing tombstones in `tests/python/test_ndnsf_di_orphan_cleanup.py`
- [X] T057 [US1] Define `RequestCoordinatorBinding`, `AuthenticatedProviderReceipt`, `ExecutionCommitCertificate`, `DeploymentLifecycleRecord`, `OrphanCleanupRecord` and `ResultRendezvousRecord` with versioned canonical serialization in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py`
- [X] T058 [US1] Add a DI-Core versioned receipt/certificate verifier that composes `ProviderExecutionLeaseTable` without changing the generic lease ABI, adding a second lease table or adding a top-level service name in `NDNSF-DistributedInference/cpp/ndnsf-di/DistributedExecutionConsistency.hpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/DistributedExecutionConsistency.cpp` and `NDNSF-DistributedInference/ndnsf_distributed_inference/deployment.py`
- [X] T059 [US1] Preserve signer/certificate/Data-name/wire-digest evidence from authenticated Targeted lease responses and reject unknown/mixed consistency payload versions in `pythonWrapper/ndnsf/service.py` and `NDNSF-DistributedInference/ndnsf_distributed_inference/deployment.py`
- [X] T060 [US1] Implement prepare-all, revalidation, commit-all, exact receipt-set certification and idempotent abort/release coordination in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/execution.py` and `NDNSF-DistributedInference/ndnsf_distributed_inference/deployment.py`
- [X] T061 [US1] Require certificate membership, current attempt/Provider boot epoch, committed unexpired lease and binding digest before Selection activates native Provider work in `ndn-service-framework/ServiceProvider.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp` and `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`
- [X] T062 [US1] Implement same-identity requester recovery, durable result rendezvous, output-attempt fencing, bounded high-watermark tombstones and a Provider-owned periodic call to canonical lease cleanup plus DI session/cache/runner cleanup independent of operation entry in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/recovery.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/execution.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp` and `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`
- [X] T063 [US1] Implement APPDeployment single-writer lifecycle records, monotonic epoch/CAS fencing, complete action certificates and fail-closed destructive transitions in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/deployment.py`
- [X] T064 [US1] Run T051-T063 plus a MiniNDN phase-boundary fault smoke and record exact commands, candidate identity, fault matrix, safety/cleanup bounds and negative outcomes in `specs/111-ndnsf-di-core-app-separation/evidence/us1-distributed-consistency-gate.md`

### Compatibility and validation

- [X] T065 [US1] Map every moved legacy import to one canonical Core target in `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T066 [US1] Implement thin compatibility re-exports with usage signals and no logic in `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/exports.py`
- [X] T067 [US1] Reduce legacy `runtime_v1.py` sections migrated by T043-T046 to compatibility imports and CLI delegation only
- [X] T068 [US1] Run T031-T035 plus existing native/runtime/security characterization and record results in `specs/111-ndnsf-di-core-app-separation/evidence/us1-core-gate.md`
- [X] T069 [US1] Run Core-only import/dependency inventory and record loaded modules and forbidden count in `specs/111-ndnsf-di-core-app-separation/evidence/core-import-gate.json`
- [X] T070 [US1] Demonstrate a rollback to the Phase 2 compatibility release without schema/evidence migration and record it in `specs/111-ndnsf-di-core-app-separation/evidence/us1-rollback.md`
- [X] T071 [US1] Mark only the US1 ownership rows as enforced in `tests/fixtures/ndnsf-di-core-app-separation/ownership-matrix.json`

**Checkpoint**: The Core execution surface works independently; only a complete
authenticated receipt certificate can activate distributed work; crash,
partition, restart and lifecycle conflicts are fenced; orphan resources are
bounded; no policy redesign has occurred.

---

## Phase 4: User Story 2 - Application-Owned Planning Policy (Priority: P2)

**Goal**: Publish an external optimization SDK and inject instance-scoped
application policy after Core eligibility and before Core decision application.

**Independent Test**: A standalone wheel loaded through direct registration and
an allowlisted entry point supplies all ten Python policies plus an independent
Runner adapter registration without source
edits; each port changes its intended valid proposal, omitted ports invoke named
defaults, and Core/provider mechanisms reject every invalid proposal. A
process-local engine executes the declared decision graph over one objective and
fresh snapshot lineage; exact model constraints and all refined lifecycle,
batching, tuning and cache boundaries are enforced.

### Contract and authority tests

- [X] T072 [P] [US2] Add metric-aware objective, estimate-envelope, snapshot/engine-graph, candidate-budget, policy-state/evidence, all ten policy request/result, independent Runner adapter and optional observer round trips, decision-epoch invalidation/no-per-packet-or-token assertions and frozen default parity in `tests/python/test_ndnsf_di_engine_contract.py`, `tests/python/test_ndnsf_di_model_variant_policy.py`, `tests/python/test_ndnsf_di_provider_assignment_policy.py`, `tests/python/test_ndnsf_di_model_adapters.py` and `tests/python/test_ndnsf_di_external_optimizer_sdk.py`
- [X] T073 [P] [US2] Add deterministic fixed-policy fixtures in `tests/fixtures/ndnsf-di-core-app-separation/fixed-policy-cases.json`
- [X] T074 [P] [US2] Add cost-policy golden fixtures derived from T019 in `tests/fixtures/ndnsf-di-core-app-separation/cost-policy-cases.json`
- [X] T075 [P] [US2] Add negatives for missing objective units/normalization, stale/unknown estimates, exact-semantic replacement, out-of-set model variants, candidate-budget overflow, cross-scope admission/scheduling, adapter-incompatible batches, torn execution intents, unsafe scale/drain/unload, invalid streaming checkpoint/output epoch, tuning/cache/assignment/recovery/target bypass, sensitive-payload projection and observer-current-request mutation in `tests/python/test_ndnsf_di_engine_contract.py`, `tests/python/test_ndnsf_di_model_variant_policy.py`, `tests/python/test_ndnsf_di_policy_boundary_negative.py`, `tests/python/test_ndnsf_di_scheduling_contract.py`, `tests/python/test_ndnsf_di_tuning_cache_contract.py`, `tests/python/test_ndnsf_di_execution_intent.py`, `tests/python/test_ndnsf_di_stream_recovery.py` and `tests/python/test_ndnsf_di_least_input.py`
- [X] T076 [P] [US2] Create a standalone optimizer fixture wheel implementing all ten policies, one Runner adapter and one idempotent optional observer using public imports only; include joint model/plan/Provider cases, three model sizes, two precision profiles, state epoch/digest, scoped admission/scheduling and direct/allowlisted/partial-suite/failure tests in `tests/fixtures/ndnsf-di-external-optimizer/`, `tests/python/test_ndnsf_di_external_optimizer_sdk.py` and `tests/python/test_ndnsf_di_optimization_observer.py`
- [X] T077 [US2] Add a decision-application test proving Core performs final validation after any policy result in `tests/python/test_ndnsf_di_core_eligibility.py`

### Policy seam and migrated implementations

- [X] T078 [US2] Define Core-owned metric/estimate/objective/snapshot/graph, model-candidate, lifecycle, plan/assignment, scoped scheduling/admission, tuning/cache/recovery/target, progress/checkpoint, atomic-intent and outcome types in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/ports.py`; expose ten one-method policy protocols plus Runner adapter and observer SPIs in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/contracts.py` without Core-to-SDK import
- [X] T079 [US2] Implement Core/provider validation for hard-metric/estimate semantics, candidate bounds, exact semantics, graph/scope/adapter capability, lifecycle, plan/variant/assignment binding, lease/cache/checkpoint and all-policy results plus atomic execution-intent prepare/revalidate/commit/abort/release in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/decision_validation.py` and `core/execution_intent.py`
- [X] T080 [P] [US2] Implement instance-scoped ten-policy suite/registry, independent adapter registry and optional observer registration with duplicate/version/state-isolation rejection in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/registry.py`, `suite.py`, `adapters.py` and `observer.py`
- [X] T081 [P] [US2] Implement versioned serialized worker request/result/outcome envelopes and least-input projections excluding prompt/tensor/secret/cross-tenant data in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/worker.py`
- [X] T082 [US2] Implement bounded policy execution and separately budgeted idempotent off-path observer delivery with irrevocable late-result rejection in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/executor.py` and `sdk/observer.py`
- [X] T083 [US2] Implement opt-in `ndnsf_di.optimizers` discovery with distribution-name/version/RECORD-digest allowlisting and explicit trust diagnostics in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/loader.py`
- [X] T084 [US2] Create the closed source-symbol/caller-to-port/default/validator/evidence inventory in `specs/111-ndnsf-di-core-app-separation/contracts/decision-point-inventory.json` and a static unclassified/native-only policy-site gate in `tests/python/test_ndnsf_di_decision_point_inventory.py`
- [X] T085 [P] [US2] Implement one-role/multi-role `FixedProviderAssignmentPolicy` in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/fixed_policy.py`
- [X] T086 [P] [US2] Move current Runtime v1 and native multi-role scoring into versioned `CostProviderAssignmentPolicy` in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/provider_assignment_policy.py`
- [X] T087 [US2] Preserve exact current deterministic weight/default behavior and expose explicit policy/version evidence in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/cost_policy.py`
- [X] T088 [US2] Move sequential split ranking, proportional allocation and local plans into a reference `PartitionPlanner` returning candidate-budgeted variant-bound `PlanCandidateSet` entries in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/split_policy.py`
- [X] T089 [P] [US2] Move current role/transfer and Provider FIFO/Qwen ordering into parity defaults under `REQUEST_DAG` and `PROVIDER_LOCAL` `SchedulingPolicy.dispatch` scopes; expose adapter-capability-aware dynamic/in-flight batching, phase arbitration, fairness, bounded preemption and assignment-authorized hedging without a new built-in optimization claim in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/scheduling_policy.py`
- [X] T090 [P] [US2] Move current exact-forward, semantic, prefix and KV cache behavior into versioned `CachePolicy` implementations with closed `LOOKUP_OR_REUSE/PLACE/PREFETCH/ADMIT/RETAIN/EVICT/MIGRATE_OR_REPLICATE` kinds, separate pre-assignment/post-execution epochs and atomic state validation in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/cache_policy.py`, emitting affinity without final compute assignment
- [X] T091 [P] [US2] Move current provider admission into `PROVIDER_LOCAL` reference behavior and add parity-safe `ENGINE_REQUEST` default admission, both unable to weaken floors or reverse prior rejection, in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/admission_policy.py`
- [X] T092 [P] [US2] Move current bounded retry/replan into a transition-only `RecoveryPolicy` over Core-advertised restart/resume/output-commit/checkpoint boundaries, delegating replacements and preserving deadline/attempt/visible-output authority in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/recovery_policy.py`
- [X] T093 [P] [US2] Move deployment ranking/activation/reservation into a parity control-plane default and expose idempotent use-existing/prewarm/scale/drain/unload proposals with lifecycle epoch, readiness, cooldown/residency and active-binding guards, without claiming a built-in autoscaler, in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/deployment_policy.py`
- [X] T094 [P] [US2] Adapt FirstResponding/Random/AllSelected/cache-aware choices into one-role `ProviderAssignmentPolicy`, consume affinity, and add authorized selected-variant/plan/role-target plus multi-role assignment integration in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/provider_assignment_policy.py`
- [X] T095 [P] [US2] Move segment-size, microbatch and compression choices into versioned `ExecutionTuningPolicy` defaults, define typed/ranged/scoped transfer-chunk, prefetch-depth, compute/communication-overlap and speculative-decoding-window parameter families, and reject undeclared dispatch/capacity changes in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/execution_tuning_policy.py`
- [X] T096 [P] [US2] Move runtime/ONNX target choice into bounded `ExecutionTargetPolicy.propose` defaults for each plan-role-Provider alternative using advertised device, batch, KV/prefix and checkpoint capabilities; keep planner lookup explicit and resolve only assignment-selected adapters through `sdk/adapters.py` in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/execution_target_policy.py`
- [X] T097 [US2] Implement `ModelVariantPolicy.propose` and deterministic exact-or-compatible-set default with bounded pruning in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/model_variant_policy.py`; compose installed defaults into `DefaultOptimizationSuite` with explicit identity/state digests and no private bypass in `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/defaults.py`
- [X] T098 [US2] Export all ten protocols/default identities, metric/estimate/engine/intent/outcome types, Runner adapter/registry and optional observer SPI plus reusable external contract tests without APP/model imports in `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/__init__.py`, `sdk/contract_tests.py` and `planner/__init__.py`
- [X] T099 [US2] Update existing `planner_registry.py` and `split_planner.py` to thin compatibility targets without duplicate implementation
- [X] T100 [US2] Implement the APP-owned engine DAG with metric/estimate snapshots, candidate budgets, scoped admission/scheduling, joint variant/plan/assignment, atomic intent preparation, evidence/outcome emission and bounded re-entry in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/engine.py`; migrate APPClient/Runtime v1 callers under named defaults/Core validation in `app.py`, `client.py` and `runtime_v1.py`
- [X] T101 [US2] Update bounded replanning to use Core progress/output-commit/checkpoint facts and selected transition-only recovery hooks, reject duplicate/stale visible output and emit outcomes under original deadline/seed in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/recovery.py`
- [X] T102 [US2] Move LLM variant/split/cache/local-plan semantics behind candidate/variant-bound plan contracts, register Qwen alternatives and preserve tokenizer/prompt/sampling/stop exact semantics unless explicitly authorized, without Core interpreting quality, in `NDNSF-DistributedInference/ndnsf_distributed_inference/llm_stub_planner.py`
- [X] T103 [US2] Update ONNX graph/split callers to the planner-owned interface in `NDNSF-DistributedInference/ndnsf_distributed_inference/onnx_graph.py`

### Evidence and rollback

- [X] T104 [US2] Update compatibility entries for every inventoried current decision/default target, including model variant, engine orchestration, objective/snapshot lineage, score, assignment, ACK selection, deployment lifecycle, scheduling/batching, tuning, cache phase/action, backend, planner registry and split planner, in `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T105 [US2] Run all-ten-policy replacement, engine graph closure, objective/estimate semantics, joint model/plan/Provider/target cases, candidate pruning, lifecycle idempotency/cooldown/drain, scoped admission/scheduling with adapter capabilities, atomic-intent fault matrix, streaming recovery, tuning/cache, selected-adapter creation, observer idempotency/failure isolation/state lineage, least-input privacy, partial defaults, parity/replay/malicious/fallback matrices; record exact decisions/rejections in `specs/111-ndnsf-di-core-app-separation/evidence/us2-policy-gate.json`
- [X] T106 [US2] Re-run existing runtime-aware planner/campaign/scenario/cache/scheduler/backend tests plus focused external-port integrations and one standalone-optimizer MiniNDN inference smoke; record counts/durations/result paths in `specs/111-ndnsf-di-core-app-separation/evidence/us2-regressions.md`
- [X] T107 [US2] Prove Core import graph has no planner implementation or model adapter dependency in `specs/111-ndnsf-di-core-app-separation/evidence/us2-import-gate.json`
- [X] T108 [US2] Demonstrate rollback to the migrated cost policy through the compatibility interface in `specs/111-ndnsf-di-core-app-separation/evidence/us2-rollback.md`
- [X] T109 [US2] Mark planner/policy ownership rows enforced in `tests/fixtures/ndnsf-di-core-app-separation/ownership-matrix.json`

**Checkpoint**: APP/planner owns objectives; Core still owns correctness and authority.

---

## Phase 5: User Story 3 - Concurrent Request-Scoped Assignments (Priority: P3)

**Goal**: Replace ambient global placement state with immutable per-request context.

**Independent Test**: 100 overlapping paired requests with opposite assignments show zero bleed and zero environment mutation.

### Concurrency and negative tests

- [X] T110 [P] [US3] Add AssignmentContext round-trip, immutability and malformed-input tests in `tests/python/test_ndnsf_di_assignment_context.py`
- [X] T111 [P] [US3] Add paired opposite-assignment concurrency fixtures in `tests/fixtures/ndnsf-di-core-app-separation/concurrent-assignments.json`
- [X] T112 [P] [US3] Add a 100-repetition overlapping request isolation test using two independently configured optimizer suites in `tests/python/test_ndnsf_di_assignment_context_concurrency.py`
- [X] T113 [P] [US3] Add a process-environment snapshot assertion that forbids preference writes in `tests/python/test_ndnsf_di_assignment_context_concurrency.py`
- [X] T114 [P] [US3] Add mixed legacy/new caller and malformed-context tests in `tests/python/test_ndnsf_di_assignment_context_compatibility.py`
- [X] T115 [US3] Add a repository-wide negative scan for writers of `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE` in `tests/python/test_ndnsf_di_architecture_imports.py`

### Request-scoped implementation

- [X] T116 [US3] Implement immutable AssignmentContext construction/validation in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py`
- [X] T117 [US3] Make Core placement application create AssignmentContext after final validation in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/placement.py`
- [X] T118 [US3] Thread AssignmentContext explicitly through client collaboration calls in `NDNSF-DistributedInference/ndnsf_distributed_inference/client.py`
- [X] T119 [US3] Thread AssignmentContext explicitly through provider/native execution entry points in `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`
- [X] T120 [US3] Replace the global environment preference bridge with a pure legacy-record translator in `NDNSF-DistributedInference/ndnsf_distributed_inference/deployment.py`
- [X] T121 [US3] Update GUI collaboration invocation to pass explicit context through the APP/operations owner in `NDNSF-DistributedInference/ndnsf_distributed_inference/gui.py`
- [X] T122 [US3] Replace `roleProviderPreferenceFromEnv()` with explicit request-scoped role assignment parameters in `pythonWrapper/src/ndnsf/_ndnsf.cpp` and expose them without new wire names in `pythonWrapper/ndnsf/service.py`
- [X] T123 [US3] Preserve attempt epoch, original deadline and exclusion lineage across context-based replanning in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/recovery.py`

### Evidence and rollback

- [X] T124 [US3] Migrate environment-based callers in `examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py` and `tests/python/test_ndnsf_di_runtime_aware_campaign.py`, remove every remaining reader/writer of `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE`, and record the exact negative scan in `specs/111-ndnsf-di-core-app-separation/evidence/global-state-removal.md`
- [X] T125 [US3] Run T110-T115 under thread scheduling stress and record 100 paired repetitions in `specs/111-ndnsf-di-core-app-separation/evidence/us3-concurrency-gate.json`
- [X] T126 [US3] Run targeted normal/Targeted/collaboration security regressions and record results in `specs/111-ndnsf-di-core-app-separation/evidence/us3-security-gate.md`
- [X] T127 [US3] Demonstrate legacy helper rollback still delegates to the same Core implementation in `specs/111-ndnsf-di-core-app-separation/evidence/us3-rollback.md`
- [X] T128 [US3] Update assignment-context compatibility entries and usage signals in `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T129 [US3] Mark request-scoped assignment ownership rows enforced in `tests/fixtures/ndnsf-di-core-app-separation/ownership-matrix.json`

**Checkpoint**: Concurrent requests have explicit isolated assignments; ambient placement state is gone.

---

## Phase 6: User Story 4 - Deploy, Use, Package, and Migrate (Priority: P4)

**Goal**: Complete the operator and application deploy/use workflow, then physically isolate APP/planner/model/ops owners, migrate callers, and preserve bounded compatibility/evidence.

**Independent Test**: From a clean persistent state root, an operator validates, resolves, dry-runs and applies one immutable revision, waits for READY/ACTIVE, an application submits and reopens a durable request handle across process restart, then the operator drains and retires the revision; owner-specific profiles build, the legacy aggregate works, historical evidence is unchanged, and a new candidate identity is produced.

### Deployment and invocation workflow

- [X] T130 [P] [US4] Add `DeploymentDefinition`, immutable `DeploymentRevision` and external `ArtifactReference` round-trip, digest-stability, secret-redaction and negative-path tests in `tests/python/test_ndnsf_di_deployment_workflow.py`
- [X] T131 [P] [US4] Add owner-only locking/permissions, identity namespace/traversal/symlink/cross-tenant isolation, schema/version, torn-write, corruption, quota, protected request-spool retention/cleanup and restart recovery tests for the fixed APP RuntimeJournal in `tests/python/test_ndnsf_di_runtime_journal.py`
- [X] T132 [P] [US4] Add `APPDeployment.validate/resolve/plan/apply/status/wait/rollback/drain/delete` idempotency, restart and terminal-failure tests in `tests/python/test_ndnsf_di_deployment_workflow.py`
- [X] T133 [P] [US4] Add `APPClient.submit/open_request/status/wait/result/cancel/stream` durable-handle tests, including missing/expired/tampered protected request envelopes, plus synchronous and process-local Future compatibility tests in `tests/python/test_ndnsf_di_request_handle.py`
- [X] T134 [P] [US4] Add provider revision binding, signed READY/ACTIVE evidence, warmup failure, rolling upgrade, rollback-as-new-epoch and drain-deadline tests in `tests/python/test_ndnsf_di_revision_upgrade.py`
- [X] T135 [P] [US4] Add Python/operations-CLI semantic parity and metadata-only `deploy_plan()` compatibility tests in `tests/python/test_ndnsf_di_ops_cli.py` and `tests/python/test_ndnsf_di_app_sdk_compatibility.py`
- [X] T136 [US4] Define deployment, artifact, lifecycle, operation and durable request-handle value types in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/contracts.py` without adding wire protocol names
- [X] T137 [US4] Implement the fixed append-only, checksummed, owner-only, locked, versioned APP RuntimeJournal plus protected request-envelope spool/references with atomic recovery, retention cleanup and bounded compaction in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/runtime_journal.py`
- [X] T138 [US4] Implement `APPDeployment` validation, revision resolution, dry-run, apply/reconciliation, status/wait, rollback, drain and delete over existing Core deployment/lease mechanisms in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/deployment.py`
- [X] T139 [US4] Implement generic externally launched provider-agent registration followed by revision-scoped artifact staging, permission/adapter/capacity checks, signed readiness publication, activation, drain and shutdown hooks in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py`
- [X] T140 [US4] Implement journal-backed APPClient request handles with protected-envelope persistence/reopen and rename the metadata-only preparation path to `prepare_session()` while retaining bounded `deploy_plan()`/`DeploymentSession` aliases in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py` and the thin legacy adapters
- [X] T141 [US4] Define stable separate revision state, instance phase, reason-coded condition, deployment-operation and request-status/event schemas in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/status.py`
- [X] T142 [US4] Implement thin operations commands for validate, resolve, plan, apply, status, wait, rollback, drain, delete and request submit/status/wait/result/cancel/stream by delegating to APP APIs in `NDNSF-DistributedInference/ndnsf_distributed_inference/ops/cli.py`
- [X] T143 [US4] Add one canonical no-secret, externally mounted-model deployment and durable invocation example under `examples/python/NDNSF-DistributedInference/deployment_workflow/`
- [X] T144 [US4] Run the clean-profile MiniNDN validate-to-delete operational gate, including APP/client restart, one upgrade/rollback and drain, and record commands, identities and results in `specs/111-ndnsf-di-core-app-separation/evidence/us4-deployment-workflow-gate.md`

### APP, model, and operations ownership

- [X] T145 [P] [US4] Add APP façade/engine/suite/adapter/observer, objective/estimate/snapshot/graph and execution-intent constructor/configuration characterization in `tests/python/test_ndnsf_di_app_sdk_compatibility.py` and `tests/python/test_ndnsf_di_engine_contract.py`
- [X] T146 [P] [US4] Add Python/native `RunnerAdapter` registry, public-header and missing-dependency tests in `tests/python/test_ndnsf_di_model_adapters.py` and `tests/native-external-runner/`
- [X] T147 [P] [US4] Add operations CLI owner/delegation tests in `tests/python/test_ndnsf_di_ops_cli.py`
- [X] T148 [US4] Move APPClient façade, explicit suite/observer selection, engine admission, joint model-candidate/partition/cache-affinity/assignment/target, request-DAG scheduling, intent preparation and outcome emission to `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, reusing `app_sdk/engine.py`
- [X] T149 [P] [US4] Move APPProvider façade and provider-local admission, adapter-capability batching/scheduling, tuning/cache/target execution, Runner creation and progress/output/checkpoint reporting to `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py`, resolving omissions through named defaults and passing proposals through Core/provider validators
- [X] T150 [P] [US4] Move APPDeployment façade/workflow, idempotent lifecycle with readiness/cooldown/residency/drain, metric/estimate projection and policy configuration to `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/deployment.py` and `app_sdk/policy.py`, leaving legacy modules thin
- [X] T151 [P] [US4] Move DistributedInferenceController and APPController façade ownership to `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/controller.py`, leaving `controller.py` thin
- [X] T152 [US4] Move GUI ownership to `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/gui.py`, add APP SDK canonical exports in `app_sdk/__init__.py`, and keep legacy `app.py`/`gui.py` as thin compatibility adapters
- [X] T153 [P] [US4] Move Python ONNX graph/executor behind explicit registration in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/onnx/__init__.py` and physically move `OnnxRuntimeModelRunner.*` to `NDNSF-DistributedInference/cpp/adapters/onnx/` while preserving namespace, symbols and characterized behavior
- [X] T154 [P] [US4] Move Python Qwen pilot/generation behavior behind explicit registration in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/__init__.py` and physically move `QwenGenerationSession.*` to `NDNSF-DistributedInference/cpp/adapters/qwen/` while preserving namespace, symbols and characterized behavior
- [X] T155 [P] [US4] Move llama runtime implementation behind explicit registration in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/llama/__init__.py`
- [X] T156 [US4] Move production CLI/status/metrics/release dispatch to `NDNSF-DistributedInference/ndnsf_distributed_inference/ops/cli.py` and simulated Runtime v1 utilities to `ops/contract_smoke.py`
- [X] T157 [US4] Leave `runtime_v1.main` as a compatibility delegation with no command implementation in `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py`

### Installation profiles and package boundaries

- [X] T158 [P] [US4] Define and build `ndnsf-di-core` from `NDNSF-DistributedInference/packaging/python/core/pyproject.toml` with ownership limited to `ndnsf_distributed_inference/core/**` plus public native Core headers/libraries
- [X] T159 [P] [US4] Define and build `ndnsf-di-sdk`, `ndnsf-di-app` and `ndnsf-di-planner` from `NDNSF-DistributedInference/packaging/python/sdk/pyproject.toml`, `app/pyproject.toml` and `planner/pyproject.toml` with disjoint namespace ownership
- [X] T160 [P] [US4] Define and build `ndnsf-di-adapter-onnx`, `ndnsf-di-adapter-qwen` and `ndnsf-di-adapter-llama` from matching metadata under `NDNSF-DistributedInference/packaging/python/adapters/` without model weights
- [X] T161 [P] [US4] Define `ndnsf-di-ops` and the `ndnsf-distributed-inference` compatibility aggregate under `NDNSF-DistributedInference/packaging/python/ops/` and `compat/`, with only compat owning the root `__init__.py` and legacy modules
- [X] T162 [US4] Convert `NDNSF-DistributedInference/setup.py` into a bounded legacy build delegation or removal instruction and enforce one canonical distribution/version/contract compatibility matrix in `NDNSF-DistributedInference/packaging/python/compat/pyproject.toml`
- [X] T163 [US4] Route console entry points to the operations owner while retaining command names in `NDNSF-DistributedInference/setup.py`
- [X] T164 [US4] Add clean isolated wheel build/install/uninstall and `RECORD` collision tests for Core, SDK, APP/planner, model adapters, compatibility and the standalone external optimizer in `tests/python/test_ndnsf_di_installation_profiles.py`
- [X] T165 [US4] Add Core-only module/SBOM forbidden-owner checks, including zero SDK imports and no root compatibility `__init__.py`, to `tests/container/unit/test_ndnsf_di_core_image.py`
- [X] T166 [US4] Update OCI build inputs to choose explicit owner profiles without model weights in `packaging/ndnsf-di-container/oci/Dockerfile.gpu`
- [X] T167 [US4] Update sealed dependency/source manifest generation for owner profiles in `packaging/ndnsf-di-container/oci/scripts/prepare-sealed-context.py`
- [X] T168 [US4] Update the container runtime probe source to assert Core/APP/model/ops profile identity in `packaging/ndnsf-di-container/oci/scripts/probe-runtime.py`, and validate it only through local unit/static fixtures without starting a container runtime

### Caller migration

- [X] T169 [P] [US4] Migrate Python examples to canonical APP/planner/model owner imports in `examples/python/NDNSF-DistributedInference/`
- [X] T170 [P] [US4] Migrate MiniNDN experiment imports and command ownership without changing campaign semantics in `Experiments/`
- [X] T171 [P] [US4] Migrate container/systemd/iTiger adapter imports and commands in `packaging/`, emit the immutable `RuntimeAllocationHandoff`/distinct infrastructure-handle schema from public operations contracts, and keep all concrete Slurm submission/process supervision in the existing Spec 110 adapter
- [X] T172 [P] [US4] Migrate Python tests to canonical owner imports while retaining dedicated compatibility tests in `tests/python/`
- [X] T173 [P] [US4] Split Core, ONNX-adapter and Qwen-adapter C++ source/target ownership, install public runner headers, and build the out-of-tree native runner fixture without changing public symbols in root `wscript`, `examples/wscript`, `tests/wscript` and `tests/native-external-runner/`
- [X] T174 [US4] Regenerate compatibility caller counts and mark migrated entries in `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T175 [US4] Add the first zero-caller snapshot without deleting adapters in `specs/111-ndnsf-di-core-app-separation/evidence/compatibility-callers-snapshot-1.json`

### Documentation, identity, and rollback

- [X] T176 [P] [US4] Update Core/APP architecture, process-local engine decision graph, objective/snapshot ownership, ten-policy boundaries and anti-overfactoring map in `docs/architecture.md` and `docs/ndnsf-core-app-boundary.md`
- [X] T177 [P] [US4] Update English installation/migration/application guidance with exact-model versus model-alternative configuration and engine/suite/adapter examples in `README.md` and `NDNSF-DistributedInference/README.md`
- [X] T178 [P] [US4] Update matching Chinese installation/migration/application guidance with exact-model versus model-alternative configuration and engine/suite/adapter examples in `README_ch.md` and `NDNSF-DistributedInference/README_ch.md`
- [X] T179 [US4] Generate the post-separation local/MiniNDN candidate identity without inheriting historical claims, recording immutable `DEFERRED_TO_SPEC110` markers instead of OCI/SIF/build digests, in `specs/111-ndnsf-di-core-app-separation/evidence/post-separation-candidate.json`
- [X] T180 [US4] Recompute T002 historical evidence digests and require byte-for-byte equality in `specs/111-ndnsf-di-core-app-separation/evidence/historical-evidence-verification.json`
- [X] T181 [US4] Demonstrate clean install/run rollback to the compatibility aggregate in `specs/111-ndnsf-di-core-app-separation/evidence/us4-rollback.md`

**Checkpoint**: The canonical deployment/use lifecycle is operable and restart-safe; owner profiles are isolated and callers migrated; compatibility remains until exit gates pass.

---

## Emergency Completion Hold: Runtime Entrypoint Remediation

**Purpose**: The 2026-07-14 MiniNDN campaign exposed a migration error before
measurement. Spec 111 completion and T182-T201 closeout remain blocked until
these tasks pass. These tasks run import/unit checks only and MUST NOT start a
MiniNDN matrix.

- [X] T202 Record the completion hold and preserve the failed campaign as non-acceptance diagnostic evidence in `specs/111-ndnsf-di-core-app-separation/evidence/completion-hold.md`
- [X] T203 Add regression tests proving `APPController` is defined by `app_sdk.controller`, all public compatibility paths resolve the same class, and all maintained callers use the canonical owner in `tests/python/test_ndnsf_di_app_sdk_compatibility.py`
- [X] T204 Move `APPController` to `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/controller.py`, retain thin compatibility re-exports, update the compatibility manifest and migrate every maintained source caller without duplicating behavior
- [X] T205 Add fail-before-output Controller/Provider/User role-entrypoint import preflight with source-root-isolated Python paths to `tools/spec111/run_non_regression_campaign.py` and cover it in `tests/python/test_spec111_role_import_preflight.py`
- [X] T206 Correct the treatment failure attribution in `specs/111-ndnsf-di-core-app-separation/evidence/non-regression-campaign.md` and keep the old 20 cells immutable and ineligible for acceptance
- [X] T207 Run the focused owner/import/preflight regression and one non-network candidate role-import probe; record that baseline remains blocked until its separate `_py_repoclient` extension is available, without starting MiniNDN, in `specs/111-ndnsf-di-core-app-separation/evidence/role-entrypoint-remediation.md`

---

## Phase 7: Cross-Cutting Validation, Compatibility Exit, and Closeout

**Purpose**: Prove separation without upgrading evidence claims or hiding regressions.

- [X] T182 Re-run every static architecture, compatibility, profile and optional-import test and record counts/durations in `specs/111-ndnsf-di-core-app-separation/evidence/static-final-gate.md`
- [X] T183 Run the complete focused Python DI suite and record pass/fail/skip counts in `specs/111-ndnsf-di-core-app-separation/evidence/python-final-gate.md`
- [X] T184 Run the native C++ plan/provider/dependency/cache/Qwen suites and record counts in `specs/111-ndnsf-di-core-app-separation/evidence/native-final-gate.md`
- [X] T185 Run the frozen security matrix from T026 and record exact results in `specs/111-ndnsf-di-core-app-separation/evidence/security-final-gate.md`
- [X] T186 Run static container profile/manifest/template and post-Spec-111 iTiger handoff render tests only: fixture OCI/SIF/revision/process-map digests, revision-derived roles, same-SIF commands, read-only model/artifact/identity binds, persistent identity-partitioned state, shared node-run socket, GPU UUID mapping and scheduler/APP/request state separation; prove zero Docker/Podman/Buildah/Apptainer execution, zero OCI/SIF build and zero Slurm submission, and record package/module/static bind inventories in `specs/111-ndnsf-di-core-app-separation/evidence/container-final-gate.md`
- [X] T187 Run each frozen baseline/treatment cell in the ten-pair 60-second MiniNDN campaign exactly once with no automatic rerun and record every measured/failed cell in `specs/111-ndnsf-di-core-app-separation/evidence/non-regression-campaign.md`
- [X] T188 Compare T187 completion, failure, p50/p95, throughput, resource and queue evidence and compute median paired relative changes plus 95% paired bootstrap intervals in `specs/111-ndnsf-di-core-app-separation/evidence/performance-comparison.md`
- [X] T189 Block compatibility deletion and record rollback if correctness/completion regresses or either non-regression interval crosses the declared 5% margin in `specs/111-ndnsf-di-core-app-separation/evidence/performance-gate.md`
- [X] T190 Generate the second independent compatibility caller snapshot after all in-repository migration in `specs/111-ndnsf-di-core-app-separation/evidence/compatibility-callers-snapshot-2.json`
- [X] T191 Evaluate each compatibility entry against two zero-caller snapshots, explicit external migration evidence or user-approved expiry, rollback and evidence gates in `specs/111-ndnsf-di-core-app-separation/evidence/compatibility-exit-gate.json`
- [X] T192 Remove only compatibility entries marked eligible by T191 from `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/exports.py`
- [X] T193 Preserve non-eligible compatibility entries with explicit owner and remaining exit condition in `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T194 Re-run T182-T186 after any T192 deletion and record the post-deletion result in `specs/111-ndnsf-di-core-app-separation/evidence/post-deletion-gate.md`
- [X] T195 Verify documentation semantic parity and canonical import/command examples in `specs/111-ndnsf-di-core-app-separation/evidence/documentation-parity.md`
- [X] T196 Reconcile every FR/SC/contract/task/evidence link in `specs/111-ndnsf-di-core-app-separation/traceability.md`
- [X] T197 Run `speckit-analyze` and record the report without silently editing artifacts in `specs/111-ndnsf-di-core-app-separation/evidence/speckit-analysis.md`
- [X] T198 Run strict code-aware pre-implementation/post-implementation Spec Kit audits at the appropriate gates and record verdicts in `specs/111-ndnsf-di-core-app-separation/AUDIT.md`; the 2026-07-15 post-implementation audit correctly records BLOCK and delegates the required final re-audit to T213
- [X] T199 Run GSD verification/health and record unrelated stale-state exclusions in `specs/111-ndnsf-di-core-app-separation/evidence/gsd-final-health.md`; health exits 0 with no errors/repairs, while the retained Spec 111 baseline, deferred Spec 110 worktree and phase-34 missing summary are explicitly classified rather than force-removed or falsely completed
- [ ] T200 Finalize `specs/110-itiger-qwen-live-inference/handoffs/spec111-separation.md` with the completed Spec 111 candidate/revision/offline-gate digests, verify the appended Spec 110 bridge tasks remain accurate, and state that any iTiger execution requires a new OCI/SIF candidate and explicit authorization
- [ ] T201 Play the completion bell and record final status, tests, evidence, rollback state, residual compatibility and next step in `specs/111-ndnsf-di-core-app-separation/completion-summary.md`

### Post-implementation audit remediation

- [X] T208 Preserve terminal diagnostic cells, continue only at the next unstarted cell after a fix, permanently mark mixed-candidate continuations non-formal, and reserve exactly one clean single-candidate whole-matrix run for final acceptance in `tools/spec111/run_non_regression_campaign.py`, `tests/python/test_spec111_role_import_preflight.py` and `specs/111-ndnsf-di-core-app-separation/evidence/performance-baseline-recipe.md`
- [X] T209 Make `app_sdk.client.APPClient`, `app_sdk.provider.APPProvider` and `app_sdk.deployment.APPDeployment` the sole canonical public owners, adapt `from_config()` to the existing network runtime, preserve network deployment-session delegation, migrate maintained examples, and repair root/legacy manifest ownership in `tests/python/test_ndnsf_di_app_sdk_compatibility.py` and `NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json`
- [X] T210 Bind canonical durable `APPClient.submit/open/status/wait/result/cancel/stream` to the real NDNSF request/certificate/result path, prove synchronous/Future adapters share that identity, and reject any protected-envelope or wire-request identity split in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, the network client/wrapper path and request-handle tests
- [X] T211 Bind `APPDeployment.apply/reconcile/drain/delete` to authenticated revision- and boot-epoch-specific APPProvider readiness/action evidence instead of a caller-supplied boolean, then replace the local-lambda T144 gate with one real Controller/Provider/User MiniNDN validate-to-delete workflow
- [X] T212 After T210-T211 and all focused gates pass, generate one new candidate identity, run readiness once, run exactly one new clean single-candidate 20-cell MiniNDN matrix, and preserve its measured result without rewriting prior evidence; candidate `spec111-local-825e9b7da919-c46998d0823c` completed 20/20 and 1,200/1,200 but failed both latency gates, so it is negative evidence rather than the accepted candidate
- [X] T214 Diagnose the T212 negative result without restarting its matrix, eliminate shared cross-cell APP state and O(N^2) journal/event scans, batch ordered durable state transitions into one tear-safe transaction, and prove convergence with focused tests plus isolated 70-request MiniNDN diagnostics in `specs/111-ndnsf-di-core-app-separation/evidence/t212-negative-result-and-remediation.md`
- [X] T215 Generate a new post-remediation candidate, run its readiness exactly once per variant and one clean single-candidate 20-cell MiniNDN matrix, then recompute performance evidence without replacing T212; candidate `spec111-local-d3c6f0517f96-c46998d0823c` completed 20/20 and 1,200/1,200 but failed the p50/p95 interval gates and remains a second preserved negative result
- [X] T216 Diagnose the T215 negative result without rerunning any cell, separate native network time from APP durability time, repair protected-envelope crash durability, digest/result rereads and asynchronous persistence-failure terminalization, and run only one isolated 70-request treatment diagnostic in `specs/111-ndnsf-di-core-app-separation/evidence/t215-negative-result-and-durability-audit.md`
- [X] T217 Resolve the prospective acceptance-contract question without relabeling T212/T215: strict audit keeps SC-007's end-to-end 5% gate unchanged because splitting or weakening it after observing failures would be post-hoc result suppression; no new candidate/matrix is allowed until T218-T220 close the newly verified correctness and durability-performance gaps
- [X] T218 Complete the T137/FR-086/SC-045 RuntimeJournal contract with typed schema-version, exclusive-writer/lock-contention, read-only/unsafe-root, quota, retention and bounded compaction behavior plus restart/corruption/failure tests; do not claim durability from rename-only or volatile fallback; focused evidence is recorded in `specs/111-ndnsf-di-core-app-separation/evidence/t218-t219-durable-runtime-gate.md`
- [X] T219 Replace the underspecified six-field `RequestHandle` with the FR-090/SC-044 durable `InferenceRequestHandle` and protected `RequestEnvelopeReference`, including requester/attempt/revision/certificate/result-rendezvous identity, typed reopen failure, attempt-fenced idempotent cancellation and restart tests over the real network binding; the first focused network attempt exposed and retained a caller identity bug, and the single affected rerun passed as recorded in `specs/111-ndnsf-di-core-app-separation/evidence/t218-t219-durable-runtime-gate.md`
- [X] T220 After T218-T219 focused gates pass, optimize only the still-correct durable commit path, prove a flat bounded local microbenchmark and one isolated 70-request MiniNDN treatment diagnostic, then generate at most one new candidate and one clean 20-cell matrix; preserve every earlier negative result and stop rather than rerun if any cell is terminal; the single diagnostic passed correctness but predicted `+12.1083%/+6.5596%` p50/p95 versus readiness baseline, so the at-most-one bound was exercised with zero new candidate/matrix rather than knowingly repeating a likely rejection, as recorded in `specs/111-ndnsf-di-core-app-separation/evidence/t220-durable-performance-convergence.md`
- [X] T221 Preserve the canonical durable request ID through the dynamic-plan runtime facade's `infer_async()` path and add a real facade forwarding regression; the localized repair and 23 passing focused tests are recorded in `specs/111-ndnsf-di-core-app-separation/evidence/t221-dynamic-request-identity.md`
- [X] T222 Bind `InferenceRequestHandle` to the authenticated execution commit certificate and exact Provider membership, add an APP-to-existing-service execution-control transport, and make post-certificate `cancel()` durably request bounded attempt-fenced Provider `CANCEL` without treating process-local `Future.cancel()` as distributed success; the handle now retains the exact certificate wire/digest, cancellation reaches every unique certified Provider through the existing authenticated collaboration service, terminal state is committed only after bound Provider evidence, and focused Python/C++ tests pass as recorded in `evidence/t222-t223-cancellation-gate.md`
- [X] T223 Add negative/unit tests for missing, stale, wrong-provider and replayed cancellation evidence, then run one focused MiniNDN cancellation fault gate proving that every certified Provider receives the original request/attempt cancellation, stale attempts cannot cancel a replacement, an accepted terminal result is not revoked and cleanup leaves no survivor; the first completed invalid-timing attempt and the corrected PASS are both preserved in `evidence/t222-t223-cancellation-gate.md`, with no performance matrix started
- [X] T224 Move protected-envelope key ownership out of `RuntimeJournal` into an owner-injected secret/key provider, default production clients to an explicit operator persistent root, reject volatile `/tmp` authority unless the MiniNDN harness supplies a named test-only override, and test missing/wrong/rotated key plus restart and mounted-root behavior without embedding key bytes in journal/evidence; implemented external key/keyring/file-provider ownership, v3 key identity, explicit production roots and named test-only MiniNDN override with 70 passing focused tests in `evidence/t224-persistent-root-and-envelope-key.md`
- [X] T225 After T222-T224 pass, design and test a correctness-preserving grouped durable request/result commit that reduces barrier count without returning before both envelope and journal reference are durable; require one bounded local microbenchmark and one isolated 70-request diagnostic to predict both p50 and p95 within 5% before spending exactly one new candidate and at most one clean 20-cell matrix, with terminal-cell skip/no-rerun semantics unchanged; candidate `spec111-local-0d80d27b786f-51c95c7ec519` completed 20/20 and 1,200/1,200 but failed the p50/p95 bootstrap interval gates, so it is preserved as negative evidence in `evidence/t225-grouped-durable-performance-convergence.md` and does not unblock T213
- [ ] T213 Re-run strict structure, `speckit-analyze`, code-aware `speckit-audit`, GSD health and Spec 110 handoff only after T218-T220 and an accepted candidate; completion remains blocked unless the audit verdict is PASS

---

## Dependencies & Execution Order

### Phase dependencies

```text
Phase 1 inventory
  -> Phase 2 characterization gate
    -> US1 Core extraction
      -> US1 distributed consistency gate
        -> US2 policy seam
        -> US3 request-scoped assignment
          -> US4 deploy/use closure + packaging/caller migration
            -> final validation and bounded compatibility exit
```

- Phase 2 failure blocks all source movement.
- US2 depends on T064 proving certificate-gated activation, fencing and bounded
  cleanup as well as the remaining US1 contracts/eligibility; it does not
  require physical package split.
- US3 depends on US2 decision/application lineage.
- US4 depends on T064 and owner interfaces from US1-US3. T130-T144 close the
  canonical deployment/use workflow before packaging completion may be called
  operationally ready; T145-T181 may proceed in parallel only where they do not
  freeze an API contradicted by that workflow.
- Compatibility deletion depends on every final gate, not merely caller migration.
- T182-T201 are additionally blocked by T202-T207. The failed 2026-07-14
  campaign cannot satisfy T187 and cannot be repaired or relabeled in place.
- T198-T201 are additionally blocked by T208-T220 and T213. T208 diagnostic
  continuation never becomes formal evidence; T212 and T215 are immutable
  negative candidates. T220 deliberately generated no new candidate because
  its one diagnostic predicted another SC-007 rejection. No further candidate
  is authorized by the current task list; a future correctness-preserving
  performance revision is now T225. T222-T223 close the audit-discovered
  Provider cancellation binding and T224 closes the production persistence/key
  boundary before T225 may generate any new candidate.

### Parallel opportunities

- T002-T006 inventory collection can run in parallel.
- T013-T025 fixture and characterization work can run in parallel by file.
- US1 contract/eligibility/state/recovery tests and modules can be developed in parallel before integration.
- T051-T056 distributed consistency characterization/fault tests can run in
  parallel; T057-T063 implement against those frozen tests before T064.
- Default policy families can run in parallel after the ten-policy and adapter contracts
  and decision-point inventory exist.
- Assignment-context unit, concurrency and compatibility tests can run in parallel.
- T130-T135 deployment/revision/journal/request/readiness/CLI characterization
  can run in parallel; T136-T142 implement against those frozen contracts before
  the integrated T143-T144 example and MiniNDN gate.
- APP, model-adapter and operations migrations can run in parallel after owner interfaces stabilize.
- Documentation languages can be updated in parallel and reconciled before closeout.

## Implementation Strategy

### MVP: US1 only

1. Complete immutable inventory and characterization.
2. Extract Core contracts/mechanisms with legacy re-exports.
3. Reuse the canonical lease mechanism and prove complete-receipt activation,
   crash/partition/restart fencing and periodic orphan cleanup.
4. Prove Core-only import and native behavior.
5. Stop before changing placement policy or packaging.

### Incremental delivery

1. US1 makes Core identifiable and independently testable and locks the
   distributed execution/deployment consistency protocol before Engine work.
2. US2 removes hard-coded application objective from Core.
3. US3 removes global request-state leakage.
4. US4 first makes deployment and invocation revision-bound, durable and
   operable, then isolates installs and migrates callers.
5. Final phase measures, rolls back on regressions, then removes only eligible compatibility.

## Stop Conditions

- Any characterization or security gate fails without an understood pre-existing baseline.
- A move adds a top-level wire name, second lease authority, implicit leader,
  security bypass or non-versioned incompatible payload semantics.
- Any prepared/partial-commit request becomes executable without the exact
  authenticated receipt certificate, or more than one attempt becomes visible.
- Requester crash, partition, Provider restart or competing deployment writer
  can bypass attempt/boot/lifecycle fencing.
- An expired lease, reservation, session, cache pin or runner handle survives
  beyond the declared periodic cleanup bound in an idle Provider.
- Core must import APP/planner/model/GUI/experiment/ops implementation to pass.
- Concurrent assignments bleed or the environment variable is still written.
- Historical evidence bytes/digests change.
- A deployment is reported READY/ACTIVE without revision-bound signed evidence,
  or a process restart loses an accepted operation/request handle.
- RuntimeJournal corruption, version mismatch, concurrent-writer conflict or
  quota exhaustion silently falls back to volatile state.
- Upgrade, rollback, cancellation, drain or shutdown can create new work after
  the applicable lifecycle/request fence.
- Matched MiniNDN correctness/completion regresses or performance exceeds the 5% rollback threshold.
- A compatibility entry lacks one canonical owner, usage signal, exit condition or rollback surface.
- A remote candidate cannot pass offline gates; no Slurm submission is authorized by this spec.
- Any task starts Docker, Podman, Buildah, Apptainer, OCI/SIF construction or an
  iTiger/Slurm command instead of limiting Spec 111 to local static checks and
  MiniNDN execution.
