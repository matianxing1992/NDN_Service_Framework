# Tasks: Selection-Gated Boundary Repair and Real Fault Validation

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md),
[boundary-repair-contract.md](contracts/boundary-repair-contract.md), and
[experiment-manifest.json](experiment-manifest.json)

**Execution gate**: The withdrawn central-coordinator plan, code, tests, runner
and old `PASS` audit authorize no task. Execute only this list after the fresh
goal-reset pre-implementation audit has no blocking finding. Never rerun Spec
129.

## Phase 1: Setup and Supersession

- [ ] T001 Establish one reachable Spec 130 design by surgically removing every withdrawn central-coordinator export/caller/mode (`ConflictAdmissionCoordinator`, `DIConflictAdmissionV1`, authority/conflict-order vocabulary, centralized manifest modes and their dedicated prototype tests), adding negative import/caller checks, and freezing the new sixteen-cell manifest plus Spec 129 hash-only boundary in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{conflict_coordination.py,__init__.py}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/deployment.py`, `Experiments/{run_spec130_concurrent_fault_matrix.py,NDNSF_DI_ConcurrentFaultBoundaries_Minindn.py}`, `tests/python/{test_ndnsf_di_spec130_conflict_coordination.py,test_spec130_concurrent_fault_runner.py}`, and `specs/130-concurrent-fault-boundaries/{experiment-manifest.json,implementation-evidence.md}`; preserve unrelated dirty changes and prove no maintained caller remains. [FR-001..FR-003, FR-033, FR-036, FR-039, SC-013]

**Checkpoint**: No executable task can accidentally continue the central design;
the new manifest and frozen baseline boundary are the only accepted Spec 130
entry points.

---

## Phase 2: Foundational NDNSF/NDNSF-DI Ownership Migration

- [ ] T002 Implement the application-neutral opaque ACK-liability/targeted-decision/tombstone hooks test-first through `ndn-service-framework/ServiceUser.{hpp,cpp}`, `ndn-service-framework/ServiceProvider.{hpp,cpp}`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, and focused generic unit/binding tests; implement the corresponding NDNSF-DI adapter in `NDNSF-DistributedInference/ndnsf_distributed_inference/{client.py,provider.py,deployment.py,plan.py}` and `core/`, migrate every maintained DI caller to it, and make legacy Core DI branches unreachable pending the T008 deletion gate. Acceptance requires generic hook names/data to contain no DI/resource/model/DAG policy, ordinary ACK compatibility, exact-target recipient security, one reachable authoritative DI interpreter, and no plaintext input/key/assignment regression. [FR-027..FR-032, FR-039, SC-010, SC-011, SC-014]

**Checkpoint**: NDNSF transports and closes opaque application liabilities;
NDNSF-DI alone creates and interprets reservation, plan and assignment state.

---

## Phase 3: User Story 1 — Late Positive ACK Closure (P1)

**Goal**: Every valid late reservation-bearing positive ACK receives one exact
target negative decision even after another Provider was selected or the normal
response callback closed.

**Independent Test**: Publish real delayed and duplicated ACKs through the
generic runtime, close the original call, and verify tombstone match,
NDNSF-DI-built `NOT_SELECTED`, receipt/retry idempotency, Provider release, zero
winner change and zero late execution.

- [ ] T003 [US1] Repair post-window ACK processing and bounded tombstone retention test-first in `ndn-service-framework/ServiceUser.{hpp,cpp}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/{client.py,provider.py,deployment.py}`, `tests/unit-tests/generic-dynamic-api-selection.t.cpp`, and `tests/python/test_spec130_late_ack.py`; cover selected-provider and completed-callback paths, exact request/attempt/provider/boot/reservation/digest binding, duplicate/reorder, stale/tampered negatives, authorized-Provider/per-request/per-identity/global quota preallocation, reject-before-publish exhaustion, no active-liability eviction, horizon garbage collection, decision receipt/retransmission and zero selection/response reopening. [FR-004..FR-007, FR-031, SC-001, SC-011]

---

## Phase 4: User Story 2 — Two Requesters and Production Retry (P1)

**Goal**: Provider-local atomic reservation plus release-before-full-jitter retry
handles disjoint, identical and complementary contention without central order
or ownership carryover.

**Independent Test**: Run two maintained client instances with distinct
identities concurrently, drive real ACK/Selection messages, and verify local
unique ownership, disjoint overlap, partial release barrier, fresh attempts,
system-entropy jitter, bounded exhaustion and complete attribution.

- [ ] T004 [US2] Close the Provider-local two-Requester reservation behavior test-first in `NDNSF-DistributedInference/ndnsf_distributed_inference/{provider.py,deployment.py}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{eligibility.py,state.py}`, native Provider reservation adapters where maintained, and `tests/python/test_spec130_concurrent_requesters.py`; exercise simultaneous disjoint, same-slot and complementary partial offers with atomic capacity accounting, per-identity/request/attempt fencing, prompt `NOT_SELECTED` release, receipt-or-expiry barriers and zero central decision/caller. [FR-008..FR-012, SC-002, SC-004]
- [ ] T005 [US2] Wire `ContentionRetryController` into the maintained high-level NDNSF-DI client lifecycle test-first in `NDNSF-DistributedInference/ndnsf_distributed_inference/{client.py,plan.py}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/recovery.py`, and `tests/python/{test_ndnsf_di_core_recovery.py,test_spec130_production_retry.py}`; production uses independent system entropy while tests inject deterministic entropy, and each retry enforces release receipt/expiry, fresh attempt/tokens/input/plan/reservation state, full-jitter base/cap bounds, maximum attempts, absolute deadline and separate collision/release/backoff/exhaustion metrics. [FR-011..FR-016, FR-026, SC-002..SC-004, SC-009]

---

## Phase 5: User Story 3 — Long-Running Execution Pin (P1)

**Goal**: A live role keeps its exact resource/model binding until completion or
confirmed fenced stop; timer expiry cannot make the resource reusable.

**Independent Test**: Cross committed lease/renewal/deadline boundaries with a
real long handler and a contending Requester, then complete, cancel, lose
renewal, restart and shut down; verify stop-before-release and stale-event
fencing.

- [ ] T006 [US3] Replace timer-only committed expiry with the explicit executable pin/STOPPING/COMPLETING contract test-first in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/deployment_control.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/{provider.py,deployment.py}`, native execution lease/runner adapters where maintained, `tests/python/{test_spec129_reservation_book.py,test_spec130_execution_pin.py}`, and relevant C++ lease tests; require renewal, deadline, cancellation, completion, restart and shutdown attribution, confirmed local stop before reuse, one release, stale completion rejection and a contender blocked for the entire pin interval. [FR-017..FR-021, FR-031, SC-005, SC-006, SC-011]

---

## Phase 6: User Story 4 — Real Multi-Stage Dependencies (P1)

**Goal**: Maintained client/provider entry points execute actual chain and
fork/join stage data using direct-predecessor eligibility and no global ready
barrier.

**Independent Test**: Send opaque payloads across three- and four-Provider
processes, prove exact lineage and branch overlap, and inject missing,
tampered, replayed and stale predecessor data before downstream execution.

- [ ] T007 [US4] Wire `DependencyDrivenExecution` and authenticated stage-data transport into maintained NDNSF-DI plan/client/provider/runtime entry points test-first in `NDNSF-DistributedInference/ndnsf_distributed_inference/{plan.py,client.py,provider.py}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{execution.py,recovery.py}`, native consistency adapters where maintained, `tests/python/{test_ndnsf_di_core_execution.py,test_ndnsf_di_execution_consistency.py,test_spec130_production_dependencies.py}`, and relevant native tests; implement source, chain, fork/join, exact direct-predecessor bindings, branch overlap, terminal-role uniqueness and bounded descendant abort/release with zero ReadySet/ExecutionActivate authority. [FR-022..FR-026, FR-031, SC-007..SC-009, SC-011]

---

## Phase 7: User Story 5 — Boundary Removal and Compatibility (P1)

**Goal**: Complete behavior migration, remove old Core DI interpretation and
prove ordinary NDNSF applications remain non-reserving.

**Independent Test**: CodeGraph/caller/import/source scans plus ordinary NDNSF
and secured NDNSF-DI regressions show one DI interpreter, zero Core DI policy
references, unchanged generic ACK semantics and preserved confidentiality.

- [ ] T008 [US5] Complete the migration/deletion gate after T003–T007 by removing every remaining generic Core/binding DI capability literal, DI plan/member/role construction, reservation/pin/retry/DAG policy branch and old ReadySet/ExecutionActivate R1 authority; add executable CodeGraph/source/import scans and ordinary non-DI plus selected/unselected/input-confidentiality/token/replay regressions in `ndn-service-framework/`, `pythonWrapper/`, `NDNSF-DistributedInference/`, `tests/unit-tests/`, `tests/python/test_spec130_core_app_boundary.py`, and `examples/`; document any application-neutral retained primitive and prove no maintained caller requires the deleted semantics. [FR-002, FR-026..FR-032, SC-009..SC-011, SC-014]

---

## Phase 8: User Story 6 — Real Exact-Once MiniNDN Evidence (P2)

**Goal**: Replace probe/counter simulation with a fresh immutable sixteen-cell
campaign using distinct hosts/processes/NFDs and event-derived fault evidence.

**Independent Test**: Dry-run rejects central modes, actor colocation, missing
fault evidence, assigned counters, duplicate case IDs, existing output,
concurrent writers, source/manifest drift and Spec 129 hash drift; a live smoke
demonstrates actual message/process fault attribution before formal execution.

- [ ] T009 [US6] Implement the new exact-once single-writer manifest runner and analyzer test-first in `Experiments/run_spec130_boundary_repair_matrix.py`, `Experiments/analyze_spec130_boundary_repair.py`, and `tests/python/test_spec130_boundary_runner.py`; enforce sixteen unique cases, immutable output, no automatic/selective rerun, Spec 129 hash-only policy, actor host/NFD/PID isolation, real-fault evidence, event-derived counters, retained failed/unavailable/harness-invalid rows, and separate Payload/Mapping/new-Mapping/ACK/decision/receipt/retry/timeout/Nack/reservation/pin/release/stage/execution/safety/availability/terminal metrics with explicit unavailable ratios. [FR-001..FR-003, FR-033..FR-038, SC-012, SC-013]
- [ ] T010 [US6] Implement the workload-neutral multi-host MiniNDN scenario and production fault controls in `Experiments/NDNSF_DI_BoundaryRepair_Minindn.py` and `tests/python/test_spec130_boundary_runner.py`, launching two independent Requesters and up to four independent Providers with one NFD per host; exercise actual Provider publish delay/duplicate, concurrent requests, long handler/renewal suppression/cancel, maintained-client jitter, real chain/fork-join payloads, predecessor link/publication fault, ordinary non-DI control and DI boundary case without UAV/codec/model/workload special logic or locally assigned result counters. [FR-008, FR-015, FR-021..FR-026, FR-032..FR-035, SC-001..SC-014]

---

## Phase 9: Validation and Closure

- [ ] T011 Run the fresh preflight, all focused Spec 130 and affected Spec 129/Core/binding/security regressions, ordinary NDNSF compatibility tests, full C++ source build/test suite, forced in-place Python binding/package rebuild, CodeGraph/source ownership scans, runner dry gate and one non-formal real-fault smoke; record exact commands, counts, environment, hashes and failures without changing Spec 129 evidence in `specs/130-concurrent-fault-boundaries/implementation-evidence.md`. [FR-031, FR-032, FR-038, SC-009..SC-014]
- [ ] T012 Execute one entirely fresh complete sixteen-cell Spec 130 MiniNDN confirmation from the frozen manifest, once per cell and with no selective rerun; preserve all negative/unavailable/harness-invalid results, verify all topology/process/fault/message/reservation/pin/retry/dependency artifacts and unchanged Spec 129 hashes, and record the canonical path and measured verdict in `specs/130-concurrent-fault-boundaries/implementation-evidence.md`. [FR-001, FR-033..FR-038, SC-001..SC-014]
- [ ] T013 Reconcile every FR/SC/task against code, maintained callers, tests and formal evidence; run post-implementation Spec Kit analyze/audit and convergence, append and execute only genuine remaining tasks, document measured limitations, then freeze the new runner/result boundary in `specs/130-concurrent-fault-boundaries/{traceability.md,audit-post.md,FROZEN.md}` only when zero blocking gap remains. [FR-001..FR-039, SC-001..SC-014]

## Dependencies and Execution Order

```text
fresh goal-reset audit
  -> T001 central supersession and new manifest boundary
  -> T002 generic hook + DI adapter migration
  -> T003 late ACK
  -> T004 concurrent local reservation -> T005 production retry
  -> T006 execution pin
  -> T007 production dependencies
  -> T008 remove remaining Core DI interpretation
  -> T009 runner/analyzer -> T010 real MiniNDN scenario
  -> T011 build/binding/preflight/smoke
  -> T012 exact-once formal confirmation
  -> T013 traceability/audit/convergence/freeze
```

T004 and T006 may be developed in parallel after T002 because they have
different application state owners. T003 and T005 both touch Requester lifecycle
and must be integrated serially. T007 depends on pin semantics. T008 is a
migration-after-parity deletion gate. T009 runner mechanics may begin after T001
but must not accept a live scenario until T003–T008 close.

## Implementation Strategy

The first independently reviewable slice is T001–T003: central-design
supersession, correct ownership boundary, and late-ACK release. It is not a
completed Spec 130. T004–T008 close the runtime claims; T009–T013 close real
distributed evidence. Formal MiniNDN execution is intentionally last and is
never used as a debugging loop.

## Task Cohesion Review

Each task represents one behavioral acceptance boundary. Its test-first work,
source/binding/application integration, focused verification and local evidence
remain coalesced. Central cleanup, full build/binding, formal one-shot execution
and final convergence remain separate because each is independently reviewable
and carries materially different migration or evidence risk.
