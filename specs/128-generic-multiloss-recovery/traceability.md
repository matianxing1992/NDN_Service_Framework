# Traceability: Generic Multi-Loss Recovery

## Requirements to delivery

| Requirement | Story | Task | Acceptance evidence |
|---|---|---|---|
| FR-001, FR-002, FR-003, FR-004, FR-005 | US1 | T001 | bounded exact-name retry and lifecycle tests; SC-001, SC-002 |
| FR-006, FR-007, FR-008, FR-009, FR-010 | US2 | T002 | GF(256) two-erasure and capacity-plus-one tests; SC-001, SC-003, SC-004 |
| FR-011, FR-012, FR-013, FR-014 | US1/US2/US3 | T001..T003 | native status, pybind parity, neutral fixtures; SC-001, SC-008 |
| FR-015, FR-016, FR-017, FR-018, FR-019 | US4 | T004, T005 | frozen runner, one-shot ledger, qdisc and hash evidence; SC-007, SC-008, SC-010 |
| FR-020, FR-021 | US4 | T003..T005 | scoped neutrality audit and bounded closing claim; SC-009, SC-011 |

## Criteria to evidence

| Criterion | Evidence and verdict |
|---|---|
| SC-001 | C++ 341/341, Python 52/52, security 12/12 plus native 115/115: PASS |
| SC-002 | `confirmation03` periodic impaired 2/5: FAIL |
| SC-003 | `confirmation03` variable impaired 5/5: PASS |
| SC-004 | `confirmation03` capacity-plus-one 4/4: PASS |
| SC-005 | both zero-loss guards accepted with frozen utility thresholds: PASS |
| SC-006 | every accepted impaired run meets utility/retry limits: PASS |
| SC-007 | periodic 2/5, variable 5/5, exact intervals recorded: FAIL overall |
| SC-008 | 16 summaries, 16 CSV rows, 16 unique one-shot invocations: PASS |
| SC-009 | CodeGraph plus scoped source search; no application-specialized branch: PASS |
| SC-010 | Spec 127 before/after hash maps identical: PASS |
| SC-011 | `completion-summary.md` records scoped negative conclusion: PASS |

All requirements and tasks are mapped. Checked task state means the planned
work was executed and evidenced; it does not convert the measured aggregate
failure into a positive result.
