# Traceability: Loss and Reordering Resilience

## Requirements to Delivery

| Requirement | Story | Design/Contract | Task | Acceptance |
|---|---|---|---|---|
| FR-001, FR-021, FR-022 | US4 | Plan 7; recovery contract invariants | T003, T005 | SC-004, SC-008 |
| FR-002, FR-003, FR-004 | US1/US2 | Plan 1; recovery invariants 1-6 | T001, T002 | SC-001 |
| FR-005, FR-012 | US2 | Plan 3; reorder dispositions | T002 | SC-001, SC-005 |
| FR-006, FR-007 | US1 | Plan 2; recovery cases | T001 | SC-002 |
| FR-008, FR-009, FR-010, FR-011 | US1 | Cursor lifecycle; recovery deadline | T001 | SC-001, SC-003 |
| FR-013 | US1/US2 | Predictor idempotence invariant | T001, T002 | SC-001 |
| FR-014, FR-015 | US1/US2 | Plan 4; bounded data model | T001, T002 | SC-003 |
| FR-016 | US1/US2/US4 | Required deterministic permutations | T001-T003 | SC-001..SC-003 |
| FR-017, FR-018 | US3 | Frozen impairment evidence contract | T004, T005 | SC-004..SC-008 |
| FR-019 | US3 | RunEvidence; campaign preflight | T004, T005 | SC-005..SC-008 |
| FR-020 | US3 | Plan 6 freeze/defect closure | T005 | SC-008 |

## Success Criteria to Evidence

| Criterion | Required evidence |
|---|---|
| SC-001 | `./build/unit-tests --run_test=Stream,UavProtocolState`: 112/112; focused Python: 50/50 |
| SC-002 | Byte-exact recovery and multiple-loss fail-closed fixtures in the 112/112 C++ gate |
| SC-003 | Bounded drain/horizon/stop tests in the 112/112 C++ and 50/50 Python gates |
| SC-004 | `confirmation07/zero-loss-run-01/run-summary.json`; Spec 125 before/after hashes identical |
| SC-005 | `confirmation07/reorder-run-01` through `run-05`: 5/5 accepted |
| SC-006 | `confirmation07`: isolated loss 4/5 and combined 4/5, with exact intervals in `campaign-summary.json` |
| SC-007 | All 16 summaries separately report Payload, Mapping, retry, timeout, Nack, future-hit, and exact latency metrics |
| SC-008 | `campaign-runs.csv` has 16 unique cells; no automatic retry; source and Spec 125 hashes identical |

No requirement or task is unmapped. T005 measured evidence is rooted at
`results/spec126-loss-reorder-20260720-confirmation07`; checked task state is
supported by the commands and immutable artifacts above, not evidence by itself.
