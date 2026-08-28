# Traceability: Cross-Application Stream Generality

## Requirements to Delivery

| Requirement | Story | Design/Contract | Task | Acceptance |
|---|---|---|---|---|
| FR-001, FR-002 | US1/US2/US4 | Plan 1; workload contract | T001-T003, T005 | SC-001, SC-009 |
| FR-003 | US1 | Plan 2; periodic fixture | T002 | SC-002 |
| FR-004, FR-005, FR-006 | US2 | Plan 3; workload contract | T003 | SC-001, SC-003 |
| FR-007 | US1/US2 | Existing Spec 126 recovery contract | T002, T003 | SC-001..SC-003 |
| FR-008, FR-009, FR-010 | US3 | Plan 4; evidence contract | T001, T004 | SC-004..SC-007 |
| FR-011, FR-012 | US3 | Plan 5; experiment plan | T004, T006 | SC-004..SC-006 |
| FR-013, FR-014 | US3 | Evidence contract; stop conditions | T004, T006 | SC-007, SC-008 |
| FR-015, FR-016, FR-017 | US4 | Plan 1/7; prohibited coupling | T005, T006 | SC-008, SC-009 |
| FR-018, FR-019 | US3/US4 | Plan 6; claim boundary | T004, T006 | SC-006, SC-010 |
| FR-020 | US1/US2/US4 | deterministic gates | T002, T003, T005 | SC-001 |

## Success Criteria to Evidence

| Criterion | Planned evidence |
|---|---|
| SC-001 | deterministic workload, Mapping, recovery, order, and stop tests |
| SC-002 | periodic zero-loss run summary and exact timeline |
| SC-003 | variable zero-loss run summary, class coverage, and exact timeline |
| SC-004 | per-workload Payload/Mapping/future-hit summaries |
| SC-005 | ten combined-profile summaries and exact accepted-count intervals |
| SC-006 | Provider future-interest evidence plus absolute performance gates |
| SC-007 | strict required-field analyzer and failed-run fixtures |
| SC-008 | 12-command ledger, unique paths, source/workload/history hashes |
| SC-009 | scoped Core/binding/UAV neutrality audit |
| SC-010 | completion-summary claim language and post-implementation audit |

No requirement, criterion, or task is currently unmapped. Live evidence remains
measured in `results/spec127-cross-application-20260720-confirmation03`; checked
task state alone is never accepted as measured evidence. The campaign verdict
is negative and the shared generality claim is withheld.

## Deterministic Implementation Checkpoints

- T001: shared manifest, opaque payload, complete-sample, and traffic-ratio
  contracts pass their focused deterministic tests.
- T002: the periodic fixture passes Mapping v2 API sequencing, exact 100 ms
  cadence, pause/resume without catch-up, provider/consumer stop fencing,
  byte-exact ordered completion, all 600 measured identities, default adaptive
  consumer selection, complete status, and single-cell command-plan checks.
  The MiniNDN cell is implemented but intentionally unexecuted until T005.
- T003: the same fixture and cell harness now cover the frozen 1/2/4/8-cap
  sequence, all 600 measured variable samples, 4096-byte exact reconstruction,
  rolling Mapping announcements, generic XOR enablement, one-source recovery,
  multiple-loss fail-closed behavior, class transitions, delayed earlier-sample
  release, terminal skip attribution, and later-sample forward progress. The
  existing Core suite retains stale/delayed Mapping admission coverage. No live
  cell has been invoked.
- T004: one shared analyzer and one-shot launcher dry-freeze twelve unique
  workload/profile/repetition commands plus source, workload, and historical
  evidence hashes. It preserves failed outcomes and reports every required
  Payload, Mapping, new-Mapping, retry, timeout, Nack, future-hit, continuity,
  coverage, and latency field.
- T005: `compatibility-evidence.md` records a full 338-test C++ build, forced
  Python binding rebuild, Python Core 19/19, generality 27/27, runner 6/6, and
  security 12/12 plus focused native 112/112. Core, binding, and accepted UAV
  hashes exactly match Spec 126 confirmation07; no live cell has been invoked.
- T006: `confirmation03` executes 12/12 fresh cells once, preserves all failed
  repetitions, and keeps source/history hashes stable. Periodic zero-loss is
  accepted 1/1; periodic combined, variable zero-loss, and variable combined
  accept 0/5, 0/1, and 0/5. `completion-summary.md` closes the feature as a
  measured negative result without a cross-application generality claim.
