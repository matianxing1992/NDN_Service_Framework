# Tasks: Cross-Application Stream Generality

## Phase 1: Shared Workload and Evidence Foundation

- [x] T001 Establish deterministic, application-neutral workload manifests, opaque byte generation, complete-sample receipt assembly, shared metric vocabulary, and fail-closed validation by writing failing cases first and then the reusable fixture foundation in `examples/python/live_stream/workload_common.py` and `tests/python/test_ndnsf_live_stream_generality.py`; acceptance requires stable seeds/digests, exact 5-second warm-up plus 600-sample measured identity, zero partial/duplicate completion, explicit unavailable ratios, and no payload-content scheduling input. [FR-001, FR-005, FR-008..FR-010, FR-014]

## Phase 2: User Story 1 - Periodic Sensor Stream

- [x] T002 [US1] Deliver the independently runnable 10 Hz periodic opaque-sample provider/consumer path through Mapping v2 `announce_sample`, `prepare_sample_extent`, `publish_sample`, and default adaptive consumption in `examples/python/live_stream/workload_provider.py`, `examples/python/live_stream/workload_consumer.py`, `Experiments/NDNSF_LiveStream_Generality_Minindn.py`, and `tests/python/test_ndnsf_live_stream_generality.py`; acceptance requires byte-exact ordered one-item samples, cadence pause/resume and stop fencing tests, 600 measured identities, complete status output, and no sensor-field interpretation outside fixture generation. [FR-002, FR-003, FR-007, FR-020; SC-001, SC-002]

## Phase 3: User Story 2 - Variable Multisegment Stream

- [x] T003 [US2] Extend the same fixture and cell runner with the frozen 1/2/4/8-cap class sequence, within-class extents, 4096-byte opaque segmentation, and one generic XOR repair in `examples/python/live_stream/workload_common.py`, `examples/python/live_stream/workload_provider.py`, `examples/python/live_stream/workload_consumer.py`, `Experiments/NDNSF_LiveStream_Generality_Minindn.py`, and `tests/python/test_ndnsf_live_stream_generality.py`; acceptance requires all four classes, sample-atomic delivery, exact digest reconstruction, one-source recovery, multiple-loss fail-closed behavior, class-transition/Mapping-delay fixtures, and forward progress without Core edits. [FR-004..FR-007, FR-020; SC-001, SC-003]

## Phase 4: User Story 3 - Shared Utility and Campaign Evidence

- [x] T004 [US3] Implement and dry-validate the single-writer 12-cell campaign/analyzer in `Experiments/run_spec127_cross_application_matrix.py` and `tests/python/test_spec127_cross_application_runner.py`, reusing the verified MiniNDN/qdisc ownership patterns from `Experiments/run_spec126_loss_reorder_matrix.py`; acceptance requires 12 unique commands, frozen workload/order/source/history hashes, exact per-run Payload/Mapping/new-Mapping/retry/timeout/Nack/future-hit/continuity/latency fields, correct zero-denominator failure, exact intervals, retained failed metrics, output-reuse and concurrent-owner rejection, and `automaticRetry=false`. [FR-008..FR-014, FR-018, FR-019; SC-004..SC-008]

## Phase 5: User Story 4 - Generic Ownership and Compatibility Gate

- [x] T005 [US4] Prove the new fixtures have not specialized or regressed the accepted implementation by adding scoped-diff/call-path neutrality assertions, historical evidence hashing, and deterministic compatibility coverage in `tests/python/test_ndnsf_live_stream_generality.py`, `tests/python/test_spec127_cross_application_runner.py`, and `specs/127-cross-application-stream-generality/compatibility-evidence.md`, then passing the full build, Stream tests, Python Core/generality tests, and security contract; acceptance requires no Spec 127 edit to `ndn-service-framework/Stream.{hpp,cpp}`, Python binding, or UAV runtime, no new policy/wire/API surface, and identical Spec 125/126 evidence hashes before the live matrix. [FR-015..FR-020; SC-001, SC-008, SC-009]

## Phase 6: Frozen Execution and Closure

- [x] T006 After T001-T005 pass, execute every preregistered 60-second MiniNDN cell exactly once in one newly named `results/spec127-cross-application-*` campaign, retain failures without replacement, evaluate every SC independently for both workload families, and close only if the strict audit agrees by writing `specs/127-cross-application-stream-generality/completion-summary.md`, updating `traceability.md` and `.planning/STATE.md`, and preserving the campaign JSON/CSV/hash evidence; any reproducible defect requires a deterministic test plus a separately named complete 12-cell confirmation, while a mere threshold miss remains a negative result. [FR-012..FR-019; SC-002..SC-010]

## Dependencies and Execution Order

- T001 is the shared deterministic vocabulary and blocks both workload paths.
- T002 and T003 each deliver an independently testable workload after T001;
  they share fixture files and therefore execute sequentially in this worktree.
- T004 depends on both workload paths because its fixed schema and 12-cell
  manifest must cover both without workload-specific analyzer branches.
- T005 depends on T004's frozen source set and blocks all live commands.
- T006 is the only task authorized to start the formal MiniNDN matrix.

## Independent Acceptance

- **US1**: periodic fixture alone produces 600 ordered opaque sample receipts
  and all periodic counters through the generic API.
- **US2**: variable fixture alone exercises all four extent classes, atomic
  completion, repair, and forward progress through the same API.
- **US3**: dry-run and synthetic summaries prove one shared evidence schema,
  strict ratios, one-shot identity, and aggregation for both workloads.
- **US4**: compatibility/neutrality gate proves the feature added no Core,
  binding, UAV, wire, policy, or security specialization.

## Cohesion Audit

Six tasks represent independently reviewable outcomes: shared fixture
semantics, each workload family, shared campaign evidence, generic compatibility,
and frozen closure. Test-first work, implementation, focused verification, and
owned evidence remain together. No task exists solely for a file, one command,
or documentation bookkeeping; no parallel marker is safe because the concrete
implementation paths or frozen source identity overlap.
