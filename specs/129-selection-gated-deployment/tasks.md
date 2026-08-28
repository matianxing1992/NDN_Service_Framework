# Tasks: Spec 129 R1 Reservation-Bearing ACK and Dependency-Driven Execution

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md), [contracts/](contracts/), [quickstart.md](quickstart.md)

**Revision note**: R0 T001--T005 were implemented against the superseded
zero-reservation ACK and complete ReadySet activation contracts. All R1 tasks
below are intentionally unchecked. Existing code/tests are migration inputs,
not completion evidence.

## Phase 1: Foundational R1 Contracts and Migration Fence

**Goal**: Establish versioned R1 contracts and prevent the R0 authority path
from being confused with R1 before behavior changes.

- [x] T001 Deliver independently negotiated `DIReservationSelectionV1` and `SelectionGatedInputV1` plus versioned bounded `EncryptedRequestInput`, generic non-reserving `SelectionInputKeyOffer`, `SelectionInputKeyGrant` with conditional DI bindings, `ReservationLease`, `SelectionDecision`, `SelectionDecisionReceipt`, recipient-encrypted assignment, stage-input, abort, and decision-tombstone contracts; add Provider-targeted V2 Selection name parsing, canonical digests, malformed/oversize/unknown-version rejection, R0/R1 authority fencing, and focused round-trip/negative tests in `ndn-service-framework/NDNSFMessages.{hpp,cpp}`, `ndn-service-framework/utils.{hpp,cpp}`, `ndn-service-framework/HybridMessageCrypto.{hpp,cpp}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{contracts,state}.py`, `tests/unit-tests/generic-dynamic-api-{deployment-control,crypto-auth,targeted}.t.cpp`, and `tests/python/test_ndnsf_di_core_{contracts,state}.py`; acceptance requires all four capability combinations, input-only encoding without reservation/plan/assignment/role, fail-before-publish for input-gated Targeted fast path, no generic GPU/DAG fields in NDNSF, exact C++/Python canonical parity, distinct input/assignment/status keys, and no R0 record authorizing R1 execution. [FR-001, FR-003, FR-011..FR-017, FR-020, FR-021, FR-026..FR-029, FR-033, SC-004, SC-011, SC-013]

---

## Phase 2: User Story 1 - Redeemable Resource Offers (Priority: P1) 🎯 MVP

**Goal**: Make every positive DI ACK correspond to exactly one bounded local
reservation without starting deployment work.

**Independent Test**: Positive, negative, duplicate, quota, expiry, crash and
journal-recovery requests prove one reservation per DI reservation-bearing positive ACK and zero per
negative ACK.

- [x] T002 [US1] Replace advisory DI capability ACK handling with authorization-before-reservation, atomic tentative reservation and signed `ReservationLease`, including identity/service/global quotas, duplicate request idempotency without lease extension, negative reasons, expiry/shutdown/restart cleanup, durable release-cause counters, and test-first concurrency/failure cases in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{contracts,state,eligibility}.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/{provider,deployment}.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/{NativeProviderHandler,NativeExecutionPlan}.{hpp,cpp}`, `tests/python/test_spec129_selection_gated_core.py`, `tests/python/test_ndnsf_di_{on_demand_protocol,deployment_fencing,runtime_journal}.py`, and `tests/unit-tests/di-reservation-lease.t.cpp`; acceptance requires reserve-before-positive-ACK only under `DIReservationSelectionV1`, ordinary positive ACK with zero reservation/mandatory negative Selection, zero reservation on authorization failure or negative ACK, zero fetch/verify/load/warm/execute at ACK, no double allocation or implicit extension, and zero orphan after maximum lease horizon. [FR-002..FR-006, SC-001, SC-002]

---

## Phase 3: User Story 2 - Private Per-Provider Selection Closure (Priority: P1)

**Goal**: Close the one ACK timeout deterministically and resolve every positive
reservation through a private exact-target decision.

**Independent Test**: A multi-Provider request produces one `SELECTED` or
`NOT_SELECTED` per DI reservation-bearing positive ACK; late ACKs receive negative decisions; only the
intended selected Provider decrypts its assignment; loss falls back to expiry.

- [x] T003 [US2] Replace compact group Selection authority with one Provider-targeted decision per reservation-bearing positive ACK and implement atomic timeout closure, verified-candidate snapshot, bounded decision tombstone, immediate late-positive-ACK `NOT_SELECTED`, exact reservation/attempt/boot fencing, signed receipts, bounded exact-target retry, and event-loop race tests in `ndn-service-framework/ServiceUser.{hpp,cpp}`, `ndn-service-framework/ServiceProvider.{hpp,cpp}`, `ndn-service-framework/utils.{hpp,cpp}`, `tests/unit-tests/generic-dynamic-api-selection.t.cpp`, and `tests/unit-tests/generic-dynamic-api-collaboration-status.t.cpp`; acceptance requires no post-timeout drain, no invalid-ACK reflection, exactly one immutable logical decision per reservation, same-digest idempotency, rejection of every conflicting later decision regardless of sequence, and lease expiry after lost decisions/receipts. [FR-007..FR-014, FR-026..FR-029, SC-002, SC-003, SC-004]
- [x] T004 [US2] Implement generic `SelectionGatedInputV1` recipient-certificate hybrid encryption independently from DI reservation, plus minimum per-Provider DI assignment projections, including signed non-reserving ACK `SelectionInputKeyOffer`, distinct fresh input/assignment keys and nonces, direct input-only grants, DI-bound grants only for selected original-input roles, complete conditional AAD, wrong-recipient/name/certificate/epoch/role/plan/replay/tamper negatives, Targeted incompatibility preflight, packet plaintext scans, and key-erasure/no-plaintext-persistence checks in `ndn-service-framework/HybridMessageCrypto.{hpp,cpp}`, `ndn-service-framework/Service{User,Provider}.{hpp,cpp}`, `NDNSF-DistributedInference/ndnsf_distributed_inference/{client,plan,provider}.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/`, `tests/unit-tests/generic-dynamic-api-crypto-auth.t.cpp`, `tests/python/test_ndnsf_di_{provider_assignment_policy,execution_intent}.py`, and `examples/run_targeted_selection_confidentiality_regression.sh`; acceptance requires all capability combinations, input-only success with zero DI objects, zero plaintext input/assignment when negotiated, zero input key in REQUEST/ACK/NOT_SELECTED, intended selected-recipient decrypt success, zero cross-recipient/unauthorized-role success, bounded key lifetime, unchanged ordinary Targeted flow, one common DI plan digest, and no unrelated assignment disclosure. [FR-015..FR-017, FR-024, FR-027, FR-028, FR-033, SC-004, SC-005, SC-013]
- [x] T005 [US2] Commit selected leases and promptly release unselected/late leases end to end in NDNSF-DI, including compare-and-transition state, bounded committed execution lease, explicit release causes, Provider restart/certificate rotation, USER crash, cancellation, mixed-version rollback, and maintained-caller migration in `NDNSF-DistributedInference/ndnsf_distributed_inference/{client,provider,deployment,app}.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.{hpp,cpp}`, `tests/python/test_ndnsf_di_{deployment_workflow,request_cancellation,orphan_cleanup}.py`, and `examples/python/NDNSF-DistributedInference/native_di_tracer/`; acceptance requires atomic commit before tentative expiry, no expired-reservation resurrection, immediate matching negative release, bounded expiry fallback, stale/conflicting decision safety, and no R0 activation authority in the R1 path. [FR-006, FR-010..FR-018, FR-029, SC-002..SC-005]

---

## Phase 4: User Story 3 - Dependency-Driven Stage Execution (Priority: P1)

**Goal**: Remove the complete ReadySet/ExecutionActivate barrier and execute
each stage from its actual local and direct-data prerequisites.

**Independent Test**: A three-stage DAG pipelines correctly, independent
branches overlap, stale data cannot trigger execution, and failure/abort never
produces an accepted partial final result or orphan resource.

- [x] T006 [US3] Replace R0 ReadySet/ExecutionActivate authority with local preparation plus authenticated direct-predecessor eligibility, including canonical DAG validation, source-stage start, stage data evidence, chunk/sequence replay fencing, hard resource pin through local completion, measured pipeline overlap, downstream failure/cancellation/deadline abort propagation, terminal-output acceptance, and migration/deletion tests in `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{contracts,execution,recovery}.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/{provider,client,plan}.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/{DistributedExecutionConsistency,NativeProviderHandler}.{hpp,cpp}`, `tests/python/test_ndnsf_di_{core_execution,execution_consistency,stream_recovery}.py`, and `tests/unit-tests/distributed-execution-consistency.t.cpp`; acceptance requires zero complete-set activation messages, exact dependency gating, at least one deterministic overlap, bounded abort/release, and zero accepted partial final result. [FR-018..FR-021, FR-029, SC-006, SC-007]
- [x] T007 [US3] Implement bounded full-jitter exponential contention retry with deterministic seed tests, maximum attempts, total deadline, release-before-retry, attempt fencing, collision/backoff/exhaustion counters, and explicit probabilistic-liveness documentation in `NDNSF-DistributedInference/ndnsf_distributed_inference/{client,plan}.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/core/{eligibility,recovery}.py`, `tests/python/test_ndnsf_di_{core_recovery,scheduling_contract}.py`, and `NDNSF-DistributedInference/README{,_ch}.md`; acceptance requires `NOT_SELECTED` for every partial reservation and receipt-or-expiry before backoff/new attempt, no fixed synchronized retry, no resource carryover between attempts, bounded exhaustion, and no starvation/fairness guarantee claim. [FR-022, FR-026, SC-008]

---

## Phase 5: User Story 4 - Confidential Pull-Only Status (Priority: P2)

**Goal**: Extend the existing secure pull path to reservation and stage state
without introducing push traffic.

**Independent Test**: Authorized pull returns monotonic recipient-encrypted R1
state; all negative identity/binding/replay cases fail; no transition pushes.

- [x] T008 [US4] Complete authenticated exact-name pull-only reservation/deployment/stage status and cursor retrieval across C++ and Python, binding reservation/decision/plan/assignment/stage identities, signature-before-decrypt, unique nonce/AAD, monotonic sequence, retention gaps, adaptive bounded polling, terminal stop, and zero unsolicited transition traffic in `ndn-service-framework/Service{User,Provider}.{hpp,cpp}`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/{service,runtime_telemetry}.py`, `tests/unit-tests/generic-dynamic-api-collaboration-status.t.cpp`, `tests/python/test_{spec129_secure_status,ndnsf_collaboration_operation_status,ndnsf_di_request_handle}.py`, and `examples/run_secure_selection_status_regression.sh`; acceptance requires zero plaintext fallback, zero unauthorized/replayed admission, strict `sequence > cursor`, bounded query counts, and C++/Python parity. [FR-023..FR-025, FR-028, SC-004, SC-009, SC-011]

---

## Phase 6: User Story 5 - Compatibility and Fresh Evidence (Priority: P3)

**Goal**: Close migration, compatibility and fresh network evidence without
reusing R0 or frozen Spec 128 results.

**Independent Test**: Generic non-R1 regressions pass, public surfaces agree,
and one frozen twelve-cell MiniNDN campaign runs each cell once with complete
reservation/decision/pipeline evidence.

- [x] T009 [US5] Complete public C++/Python parity, remove maintained R1 callers of compact group Selection and ReadySet/ExecutionActivate authority, isolate any bounded non-authoritative compatibility reader with telemetry/deletion condition, and synchronize maintained English/Chinese DI documentation and examples in `ndn-service-framework/Service{User,Provider}.hpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/`, `NDNSF-DistributedInference/README{,_ch}.md`, `examples/python/NDNSF-DistributedInference/`, and `tests/python/test_spec129_binding_contract.py`; acceptance requires unchanged non-R1 generic behavior, no GPU/DAG policy in NDNSF, exact public parity, and no stale R0 authority claim. [FR-026..FR-029, SC-010, SC-011]
- [x] T010 [US5] Deliver a deterministic single-writer R1 runner/analyzer with the frozen twelve-cell manifest, unique outputs, exact-once/no-auto-retry enforcement, process ownership, source/topology/qdisc/build identity, packet plaintext input/assignment scan, fault injection, Spec 128 hash guards, and complete ACK/reservation/decision/receipt/release/retry/timeout/Nack/bytes/overlap/completion/latency attribution in `Experiments/run_spec129_selection_gated_deployment_matrix.py`, `Experiments/NDNSF_DI_SelectionGatedDeployment_Minindn.py`, `tests/python/test_spec129_selection_gated_deployment_runner.py`, and `specs/129-selection-gated-deployment/traceability.md`; acceptance requires the runner to reject incomplete/duplicate/reused/drifted/concurrent/automatically retried cells before live launch and retain every negative result. [FR-030..FR-033, SC-012, SC-013]
- [x] T011 [US5] Execute the preflight, full C++ build/test suite, forced in-place Python binding/package rebuild, focused/full Python and security regressions, migration scans, and every fresh R1 MiniNDN cell exactly once; preserve positive and negative results, run post-implementation Spec Kit analysis/audit, and write measured closeout evidence under `specs/129-selection-gated-deployment/` and one new `results/spec129-r1-*` directory only after T001--T010 pass. [FR-027..FR-033, SC-001..SC-013]

## Dependencies & Execution Order

```text
T001 contracts/fence
  -> T002 reservation ACK
  -> T003 targeted closure/late ACK
       -> T004 recipient assignment security
       -> T005 commit/release integration
            -> T006 local DAG execution
            -> T007 contention retry
                 -> T008 secure status completion
                 -> T009 parity/migration/docs
                      -> T010 frozen runner
                           -> T011 full build + fresh matrix + audit
```

T004 may develop crypto fixtures after T001 while T003 develops transport, but
T005 cannot close until both pass. T008 may develop status-only cases after
T001 but final parity depends on T005--T007 states.

## Implementation Strategy

### MVP

T001--T002 establish truthful DI reservation-bearing ACKs and bounded cleanup
without changing execution authority. This is independently testable but not
yet eligible for live MiniNDN acceptance.

### Incremental delivery

1. Establish versioned contracts and migration fence.
2. Make ACK reservations real and bounded.
3. Deliver targeted positive/negative closure and receiver privacy.
4. Commit/release R1 reservations end to end.
5. Replace global activation with local DAG eligibility and bounded retry.
6. Complete status, parity, compatibility and documentation.
7. Freeze runner, then perform one full fresh confirmation.

## Fragmentation Scan

Eleven tasks remain cohesive behavioral outcomes. Contract/test/implementation/
focused evidence for each behavior are intentionally co-located. Targeted
transport and recipient crypto remain separate because they have independently
reviewable distributed-delivery and security risks. DAG execution and retry
remain separate because retry policy can be disabled/rolled back independently
of stage correctness. No mechanical file-operation task remains.
