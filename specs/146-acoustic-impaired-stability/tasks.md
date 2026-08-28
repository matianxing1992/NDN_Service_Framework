# Tasks: Acoustic Loss/Reorder Stability

**Input**: [spec.md](spec.md), [plan.md](plan.md),
[formal evidence contract](contracts/formal-evidence-contract.md), and
[pre-implementation audit](evidence/pre-implementation-audit.md)

**Task rule**: Each task is a cohesive behavior or independently meaningful
evidence gate. Frozen Spec 144 artifacts are read-only.

## Phase 1 — Frozen baseline and blocking audit

- [X] T001 Record Spec 144 immutable result hashes, extract the loss/reorder
  failure signature, trace recovery/scheduling ownership with CodeGraph, freeze
  hypotheses and changed-file boundaries, and issue the pre-implementation
  PASS/BLOCK verdict in
  `specs/146-acoustic-impaired-stability/evidence/pre-implementation-audit.md`.

## Phase 2 — Test-first generic Core repair

- [X] T002 [US1] Enforce exclusive source ownership by adding the
  repair-before-source and processing-ownership red cases, implementing the
  fail-closed recovery transition, and passing focused exact-once validation in
  `tests/unit-tests/stream.t.cpp` and
  `ndn-service-framework/Stream.{hpp,cpp}`.
- [X] T003 [US2] Recover only timeout-eligible or terminally missing sources by
  adding one/two/over-capacity red cases, implementing bounded eligibility,
  direct source-to-group indexing, and recovery-group retirement, and passing
  focused counter-conservation validation in `tests/unit-tests/stream.t.cpp`
  and `ndn-service-framework/Stream.{hpp,cpp}`.
- [X] T004 [US3] Keep continuous scheduling and Mapping admission bounded by
  adding completed-horizon, source-first, continuous-stream, and sequential
  successor tests; implement generic lifecycle ordering and strict-successor
  incremental Mapping admission while preserving the full atomic exceptional
  path; pass focused native/Python suites in
  `ndn-service-framework/Stream.{hpp,cpp}`,
  `ndn-service-framework/common.hpp`,
  `ndn-service-framework/Service{User,Provider}.cpp`,
  `tests/unit-tests/{stream,encrypted-permission-response}.t.cpp`, and
  `tests/python/test_ndnsf_{core_streaming,live_stream_generality}.py`.

## Phase 3 — Fresh evidence path

- [X] T005 [US3] Create and test a Spec 146-only analyzer and single-owner runner
  that reuse the frozen acoustic workload/profile contract but reject Spec 144
  destinations, source drift, duplicate cells, auto retry, and metric
  non-conservation in
  `Experiments/{NDNSF_Acoustic_Stability_Minindn.py,analyze_spec146_acoustic_stability.py,run_spec146_acoustic_stability_matrix.py}`
  and `tests/python/test_spec146_acoustic_stability.py`.
- [X] T006 [US3] Rebuild the complete native source and Python binding with `-j2`,
  execute fresh non-formal zero/loss/reorder/combined diagnostics with the
  high-rate packet probe explicitly disabled, and record a PASS/BLOCK preflight
  plus frozen hashes in
  `specs/146-acoustic-impaired-stability/evidence/final-preflight.md`; BLOCK
  prevents formal execution.
- [X] T007 [US3] Execute exactly one new 16-cell MiniNDN confirmation
  (1 zero-loss, 5 loss, 5 reorder, 5 combined) using the frozen subject and
  60-second measured window under
  `results/spec146-acoustic-stability-20260725T055500Z/`; never replace or
  selectively rerun a failed cell.
- [X] T008 [US3] Analyze all cells, issue treatment and overall verdicts, run the
  post-implementation CodeGraph/neutrality audit, verify Spec 144 hashes remain
  unchanged, and close Spec 146 with honest positive and retained negative
  evidence in `specs/146-acoustic-impaired-stability/evidence/post-implementation-audit.md`
  and `specs/146-acoustic-impaired-stability/closure-report.md`.

## Successor definition

- [X] T009 Only after T008 closes, create Spec 147 for API simplification.
  Freeze symmetric C++/Python signatures and before/after examples; exclude
  Core scheduling, FEC, retry, and prefetch-algorithm changes in
  `specs/147-stream-prefetch-api-simplification/`.

## Execution order

```text
T001 -> T002 -> T003 -> T004 -> T005 -> T006 -> T007 -> T008 -> T009
```
