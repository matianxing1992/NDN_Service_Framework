# Spec186 Traceability

| Requirement / outcome | User story | Design authority | Task(s) | Validation |
| --- | --- | --- | --- | --- |
| FR-001 exact source baseline | US1 | `contracts/candidate-identity.md` | T001 | V01 |
| FR-002 canonical Tiger ownership | US1 | plan Architecture And Ownership | T002, T003, T004 | V03, V04 |
| FR-003 immutable candidate | US1 | `contracts/candidate-identity.md` | T001, T003 | V01, V02 |
| FR-004 zero-side-effect closure | US1 | candidate identity § Pre-Dispatch Closure | T003 | V02 |
| FR-005 layered change planes | US1 | plan Candidate Identity / Change Invalidation | T003, T006 | V02, V05 |
| FR-006 build and loader closure | US1 | plan T006 | T006 | V05 |
| FR-007 MiniNDN native security/readiness | US2, US3 | plan Architecture And Ownership | T004, T007, T008 | V03, V06, V08 |
| FR-008 local Qwen model boundary | US3 | `contracts/experiment-profile.md` | T008, T011 | V08, V12 |
| FR-009 Qwen cold/follow-up evidence | US3 | plan Execution Gates | T008 | V08 |
| FR-010 Tiger YOLO graph and placement | US4 | `contracts/experiment-profile.md` | T009, T010 | V09, V10 |
| FR-011 NDN dependency data path | US2, US4 | plan Architecture And Ownership | T004, T007, T010 | V03, V06, V10, V11 |
| FR-012 observed CUDA capacity/backend | US4 | experiment profile Runtime And Resource Rules | T009, T010 | V09, V10 |
| FR-013 terminal agreement | US2, US4 | candidate identity § Terminal Acceptance | T007, T009, T010 | V06, V09, V10, V11 |
| FR-014 negative coverage | US2, US4 | candidate identity § Terminal Acceptance | T004, T007, T010 | V03, V07, V11 |
| FR-015 single-run ownership and bounded cleanup | US4, US5 | candidate identity § Terminal Acceptance | T004, T010, T012 | V03, V11, V13 |
| FR-016 convergence audit | US1 | plan Design-Code Convergence Gate | T005 | V04 |
| FR-017 validation order | all | plan Execution Gates | T005–T012 | V04–V14 |
| FR-018 evidence storage | US5 | validation matrix Evidence Rules | T013 | V14 |
| FR-019 operator handoff artifacts | US5 | quickstart and evidence rules | T013 | V14 |
| SC-001 exact baseline/candidate | US1 | candidate identity | T001, T003 | V01, V02 |
| SC-002 pre-dispatch rejection | US1 | candidate identity | T003 | V02 |
| SC-003 local YOLO | US2 | profile + validation matrix | T007 | V06, V07 |
| SC-004 local Qwen3-0.6B | US3 | Qwen profile rules | T008 | V08 |
| SC-005 Tiger single/two-node YOLO | US4 | role/topology rules | T009, T010 | V09, V10 |
| SC-006 dependency-negative | US4 | terminal acceptance | T010 | V11 |
| SC-007 independent reuse | US5 | invalidation matrix | T012 | V13 |
| SC-008 reproducible handoff | US5 | quickstart and evidence rules | T013 | V14 |

## Evidence Status Vocabulary

`NOT_STARTED`, `IMPLEMENTED`, `EXECUTED`, `VERIFIED`, `LOCAL_CPU_PASS`,
`SINGLE_NODE_GPU_PASS`, `EXPECTED_REJECTION_PASS`, `BLOCKED_AFTER_BOUNDARY` and
`WAITING_EXTERNAL_INPUT` are scoped states. No state is promoted without the
candidate digest, command, run identity and declared evidence fields.
