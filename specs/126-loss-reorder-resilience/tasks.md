# Tasks: Loss and Reordering Resilience

## Phase 1: Cursor Recovery Correctness

- [x] T001 [US1] Establish one authoritative cursor-attempt lifecycle and group-scoped exactly-once recovery by first adding deterministic timeout/Nack/Data/validation/FEC/stop permutations, then minimally repairing the existing implementation, exposing bounded aggregate outcomes, and passing the focused gates in `ndn-service-framework/Stream.{hpp,cpp}`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/streaming.py`, `tests/unit-tests/stream.t.cpp`, and `tests/python/test_ndnsf_core_streaming.py`; acceptance requires byte-exact one-source recovery, fail-closed multiple-source loss, exact-name bounded retry, one terminal cursor outcome, group-idempotent predictor training, and zero post-stop mutation.

## Phase 2: Reordered Media Delivery

- [x] T002 [US2] Make valid Mapping/source/repair and validation completion order irrelevant while preserving canonical decoder order by adding block/group-crossing and repair-first/late-source fixtures, correcting only demonstrated Core/UAV reorder ownership defects, and passing focused C++/Python validation in `ndn-service-framework/Stream.{hpp,cpp}`, `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`, `tests/unit-tests/stream.t.cpp`, `tests/unit-tests/uav-protocol-state.t.cpp`, and `tests/python/test_ndnsf_uav_unified_video.py`; acceptance requires zero duplicate APP delivery, zero out-of-order decoder input, bounded pending depth, explicit deadline/overflow skip, and unchanged rejection of stale or malformed Data.

## Phase 3: Compatibility and Security Regression

- [x] T003 [US4] Prove the loss/reorder upgrade preserves the accepted contract by running and, only for Spec 126 regressions, repairing golden Mapping v2, C++/Python API parity, full Stream/UAV, and security gates; record exact commands plus a before-campaign hash manifest of untouched Spec 125 evidence in `tests/unit-tests/stream.t.cpp`, `tests/python/test_ndnsf_core_streaming.py`, `tests/python/test_ndnsf_uav_unified_video.py`, `tests/run_uav_stream_security_contract.py`, `specs/126-loss-reorder-resilience/compatibility-evidence.md`, and `results/spec126-loss-reorder-*/evidence-hashes.json`; the matched zero-loss network regression remains one frozen T005 command, and no new wire version, public mode, codec parsing in Core, or modification of `results/spec125-*` is admissible.

## Phase 4: Frozen MiniNDN Boundary Matrix

- [x] T004 [US3] Implement and dry-validate one single-writer Spec 126 campaign path: add a narrow runner-owned impairment hook that discovers the actual Mininet endpoint interfaces and applies/captures the verified bidirectional qdiscs before apps start, then add a wrapper that creates the 16-command frozen manifest, refuses reused directories or concurrent cleanup ownership, preserves every invocation, and emits strict run/cell summaries with exact completion intervals in `Experiments/NDNSF_UAV_GUI_Minindn.py`, `Experiments/run_spec126_loss_reorder_matrix.py`, focused campaign unit tests under `tests/python/`, and `specs/126-loss-reorder-resilience/experiment-plan.md`; the dry gate must prove effective-profile parsing, wrong-parent/handle fail-closed behavior, one-invocation identity, missing-evidence failure, no automatic retry, source freeze, and full SC-004..SC-008 metric coverage before any live command starts.

- [x] T005 [US3] After T001-T004 and all deterministic/security/build gates pass, execute every frozen 60-second MiniNDN command exactly once, retain failures and invalid runs without replacement, analyze SC-004..SC-008, and close only reproducible implementation defects through deterministic tests plus a separately named complete confirmation series in `results/spec126-loss-reorder-*`, `specs/126-loss-reorder-resilience/completion-summary.md`, `specs/126-loss-reorder-resilience/traceability.md`, and `specs/126-loss-reorder-resilience/audit.md`; finish with full regression, source/evidence hash verification, strict Spec Kit audit, and GSD state synchronization.

## Dependencies

- T001 establishes cursor/recovery authority required by every impaired run.
- T002 depends on T001 terminal semantics and closes decoder-order behavior.
- T003 depends on T001-T002 and blocks any campaign if deterministic
  compatibility or security regresses.
- T004 may be developed alongside deterministic implementation but its live
  execution gate depends on T003.
- T005 depends on all prior tasks and is the only task authorized to start the
  frozen network matrix.

## Independent Acceptance

- **US1**: deterministic loss/recovery permutations close with one terminal
  cursor outcome and byte-exact or fail-closed recovery.
- **US2**: deterministic arrival permutations preserve one APP delivery and
  canonical decoder order within bounded memory/deadline rules.
- **US4**: Mapping/API/security hashes and the matched zero-loss run remain
  compatible without touching historical Spec 125 evidence.
- **US3**: the preregistered 16-command matrix runs once and reports every
  outcome against the frozen thresholds.

## Cohesion Audit

The five tasks are behavioral outcomes: recovery authority, reordered media,
compatibility/security, campaign admissibility, and frozen execution/closure.
Test-first work, implementation, focused verification, and directly owned
evidence remain in the same task. No task exists solely for one file, one test
command, one counter, or documentation bookkeeping.
