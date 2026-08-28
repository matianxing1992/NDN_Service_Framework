# Spec 133 Traceability

| Intent / requirement | Design authority | Tasks | Acceptance evidence |
|---|---|---|---|
| Exact synchronous historical subject; frozen earlier evidence (`FR-001`, `FR-002`, `FR-003`, `FR-017`, `SC-001`) | `plan.md` subject identity and rollback boundary | T001, T002, T006 | Subject manifest, patch allowlist, tree/binary/linkage audit |
| Complete synchronous-stage visibility (`US1`, `FR-004`, `FR-005`, `FR-006`, `FR-007`, `FR-008`, `SC-002`) | Stage measurement contract | T003–T006 | Unit/contract fixtures and complete two-peer smoke trace |
| Bounded instrumentation and logging perturbation (`FR-009`, `SC-003`) | Three-arm overhead admission | T002, T007, T008 | Hash-bound A-vs-B, B-vs-C, and A-vs-C receipt |
| Exactly five once-only bidirectional rates with one Face/io_context execution thread per peer (`US2`, `FR-010`, `FR-011`, `SC-004`) | Corrected serial campaign contract | T006–T009 | Preserved Spec 134 negative receipt, verified Spec 133 dual-prefix routes, admitted fresh three-arm preflight, and five immutable receipts at 200/400/600/800/1000 |
| Strict accounting and complete outputs (`FR-012`, `FR-013`, `FR-014`, `FR-015`, `FR-016`, `SC-005`, `SC-006`) | Data model and analyzer contract | T006, T007, T010, T011 | Schema/accounting checks and required CSV/JSON/Markdown outputs |
| Evidence-backed bottleneck or explicit inconclusive verdict (`US3`, `SC-007`, `SC-008`) | Two-signal bottleneck rule | T009–T011 | Ranked tables linked to raw counters/spans and the observed rate boundary |

Coverage is complete: all 17 functional requirements, 8 success criteria, and
3 user stories have a task and an evidence path. No task is unmapped.
