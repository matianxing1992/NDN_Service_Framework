# Tasks: Real DI Validation Gates

**Input**: `spec.md`, `plan.md`, `research.md`, `data-model.md`,
`experiment-plan.md`, and `contracts/`

**Execution rule**: Each task is a cohesive behavioral slice. Its tests are
written to fail first, implementation follows, and the task closes only when
its focused gate and evidence pass.

## Phase 1: Foundational Contracts

**Goal**: establish machine-checkable identities and policies shared by all
four gates.

- [x] T001 [US1] Implement the versioned fidelity, model/workload identity, and fail-closed aggregate contracts with negative fixtures for missing, malformed, stale, contradictory, cross-run, cross-revision, skipped, and lower-fidelity evidence in `Experiments/ndnsf_validation/fidelity.py`, `Experiments/ndnsf_validation/workload.py`, `tests/test_validation_fidelity.py`, and `specs/165-real-di-validation-gates/evidence/t001-fidelity-contract.md`.

- [x] T002 [US1] Turn `Experiments/NDNSF_Run_Minindn_Quick_Checks.py` into an explicitly tiered diagnostic source and implement `Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py` as the default fail-closed Gate A-D aggregate, proving that unit/fake/socket/startup cases remain useful but cannot authorize deployment in `tests/test_local_deployment_gate.py` and `specs/165-real-di-validation-gates/evidence/t002-default-aggregate.md`.

**Checkpoint**: Gate A independently rejects every evidence substitution and
the default command names all mandatory Gate A-D cases.

## Phase 2: Progress-Driven Deadline Semantics

**Goal**: deliver Gate D before long real-model executions depend on it.

- [x] T003 [US4] Implement the generic deterministic progress-admission and dual-deadline state machine, including authenticated binding, monotonic epoch/sequence/work admission, renewable idle deadline, immutable hard deadline, first-terminal-wins, and distinct `STALLED`/`HARD_TIMEOUT` outcomes in `Experiments/ndnsf_validation/deadlines.py` and `tests/test_progress_deadline.py`; close valid advance, duplicate, reorder, forgery, wrong binding, no-progress stall, continuous-progress hard cap, cancellation/completion races, and post-terminal cases without wall-clock sleeps in `specs/165-real-di-validation-gates/evidence/t003-progress-deadline.md`.

- [x] T004 [US4] Integrate admitted progress with the existing NDNSF collaboration operation-status path while preserving the ownership boundary—generic validation/deadlines in NDNSF and phase/model interpretation in NDNSF-DI—and prove a long preparation survives its original idle window while a stalled one terminates in `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`, the minimum necessary `ndn-service-framework/ServiceUser.*` or `ServiceProvider.*` files, `tests/test_progress_deadline_integration.py`, and `specs/165-real-di-validation-gates/evidence/t004-progress-integration.md`.

**Checkpoint**: Gate D passes deterministically and no progress event can move
or renew the wrong invocation.

## Phase 3: Real Qwen3 MiniNDN Proof

**Goal**: deliver Gate B with real processes, real model weights, repeated
multi-token generation, complete metrics, and full lineage.

- [x] T005 [US2] Add canonical invocation identity and append-only admitted/rejected lineage validation for Request, ACK, Selection, operation status, intermediate role execution, and Response, including wrong request/attempt/plan/model/provider-role, duplicate, reordered, and post-terminal negative cases in `Experiments/ndnsf_validation/lineage.py`, `tests/test_validation_lineage.py`, and `specs/165-real-di-validation-gates/evidence/t005-lineage.md`.

- [x] T006 [US2] Extend `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` to resolve only the pinned local `Qwen/Qwen3-0.6B` snapshot, consume the canonical workload manifest, run one User, one Controller/security carrier, and at least three Provider roles in real MiniNDN, and retain two prompts with one warmup plus three measured invocations each, at least eight ordered token events, full answers, TTFT, per-token latency, total latency, token count, tokens/s, explicit placement, fallback count, failures, and lineage in `tests/test_qwen3_minindn_gate.py` and `specs/165-real-di-validation-gates/evidence/t006-qwen3-minindn.md`.

- [x] T007 [US2] Add a fail-closed MiniNDN evidence validator that checks process/network provenance, six measured invocation records, token/metric completeness, immutable model/workload/source identity, backend consistency, and all lineage bindings, then wire it into the default aggregate in `Experiments/ndnsf_validation/fidelity.py`, `Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py`, `tests/test_qwen3_minindn_evidence.py`, and `specs/165-real-di-validation-gates/evidence/t007-minindn-admission.md`.

**Checkpoint**: Gate B alone proves the minimum real Qwen3 workload and rejects
every incomplete or mismatched request.

## Phase 4: Same-Workload Candidate Container

**Goal**: deliver Gate C without weakening the host workload or hiding missing
deployment prerequisites.

- [x] T008 [US3] Execute the exact canonical workload digest inside the candidate container with explicit immutable image identity, model mount identity, resource limits, backend placement, exit/OOM state, and the same metrics/lineage schema as Gate B; make missing image/model/backend evidence a blocking failure and prove workload/image/resource substitutions are rejected in `Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py`, `Experiments/ndnsf_validation/workload.py`, `tests/test_candidate_container_gate.py`, and `specs/165-real-di-validation-gates/evidence/t008-container.md`.

**Checkpoint**: Gate C independently demonstrates that the deployable artifact
runs the same workload, not merely that it starts.

## Phase 5: Local Closure

**Goal**: make the complete local gate reproducible and explicitly keep remote
experiments out of scope.

- [x] T009 [US1] Run the complete default local Gate A-D command from `quickstart.md`, retain one unique run with matching JSON/Markdown accounting and all failures/skips, verify nonzero behavior for mandatory prerequisite and evidence faults, update `specs/165-real-di-validation-gates/quickstart.md` if the delivered CLI differs, produce `specs/165-real-di-validation-gates/evidence/t009-local-closure.md`, rerun the strict Spec Kit audit into `specs/165-real-di-validation-gates/audit.md`, and confirm that the command performs no TigerCluster submission while reporting external validation eligibility only after all local gates pass.

- [x] T010 [US2] Make the blocking MiniNDN profile fail closed on simulated
  runtimes and incomplete Qwen3 campaign identity; fix its topology and
  convergence arguments, and require all three role selections plus per-stage
  artifact-readiness/execution markers. Record the current recheck in
  `specs/165-real-di-validation-gates/evidence/t010-minindn-first-strict-recheck-20260801.md`.

## Phase 6: Content-Addressed Experiment Retention

**Goal**: preserve reusable model preparation without letting each result run
own another multi-gigabyte payload tree.

- [x] T011 [US1] Install Qwen stage bundles in a repository-local
  content-addressed store using same-filesystem hard links, replace run-local
  stage directories with relative links, emit a retention manifest with exact
  byte/file accounting, migrate the accepted 6.0 GB bundle without copying,
  and cover seeding, source-run deletion, direct reuse, and fail-closed store
  constraints in `Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py`,
  `tests/python/test_spec165_gate_runner.py`, and
  `specs/165-real-di-validation-gates/evidence/failure-and-retention-audit-20260801.md`.

## Dependencies and Execution Order

```text
T001 -> T002
T001 -> T003 -> T004
T001 -> T005 -> T006 -> T007
T002 + T004 + T007 -> T008
T002 + T003 + T004 + T005 + T006 + T007 + T008 -> T009
T006 + T007 -> T010 -> (current-source full aggregate)
T010 -> T011 -> (future runs reference shared bundle)
```

- T003 and T005 may proceed in parallel after T001 because they own independent
  generic state machines.
- T006 depends on lineage and deadline contracts because the real run must
  exercise them rather than add them later.
- T008 depends on the stable host workload/evidence contract; it must not
  invent a container-only substitute.
- T009 is the only deployment-authorizing local closure task.

## Requirement Traceability

| Tasks | Requirements | Success criteria |
|---|---|---|
| T001-T002 | FR-001–FR-008, FR-033–FR-035 | SC-001, SC-009, SC-010 |
| T003-T004 | FR-025–FR-032 | SC-007, SC-008 |
| T005 | FR-018–FR-021 | SC-004, SC-005 |
| T006-T007 | FR-009–FR-021, FR-033–FR-034 | SC-002, SC-003, SC-004, SC-009 |
| T010 | FR-036 | SC-002, SC-004, SC-009 |
| T011 | FR-037 | SC-011 |
| T008 | FR-022–FR-024, FR-033–FR-034 | SC-006, SC-009 |
| T009 | FR-004–FR-008, FR-033–FR-035 | SC-001–SC-010 |

All 37 functional requirements and all 11 success criteria have an
implementation and acceptance owner.
