# Traceability: Spec 131

## Functional Requirements

| Requirement | Design/Contract | Tasks |
|---|---|---|
| FR-001 | Scope Boundary; contract 3/5/9 | T001-T003 |
| FR-002 | Version Boundary; contract 1 | T001, T004 |
| FR-003 | Boost 1.71 build compatibility contract; runtime architecture; failure rules | T001, T004 |
| FR-004 | Formal Matrix; execution contract | T003-T006 |
| FR-005 | Subjects and Deliberate Difference; contract 3 | T001, T006 |
| FR-006 | Fixed Controls; contract 1 | T001, T004 |
| FR-007 | Formal Matrix; campaign model | T003-T006 |
| FR-008 | Fixed Controls; contract 5 | T002, T004-T006 |
| FR-009 | Fixed Controls; contract 5 | T002, T005-T006 |
| FR-010 | Timing/Payload; contract 4 | T002, T005-T006 |
| FR-011 | Fixed Controls; contract 7 | T002-T003, T005-T006 |
| FR-012 | Timing Semantics; contract 3 | T002, T007 |
| FR-013 | PublicationObservation; contract 8 | T002, T005-T007 |
| FR-014 | Outcomes; summary contract | T002-T003, T005-T007 |
| FR-015 | Outcomes; summary contract | T002-T003, T006-T007 |
| FR-016 | Runtime Architecture; contract 3/5/8 | T002, T005-T006 |
| FR-017 | CampaignManifest; contract 1/10 | T003-T004 |
| FR-018 | Failure Rules; contract 10 | T003-T007 |
| FR-019 | Outcomes; contract 9/11 | T003, T005-T007 |
| FR-020 | Outcomes; research Decision 12 | T003, T007 |
| FR-021 | Validation Flow; quickstart 2-4 | T001, T004 |
| FR-022 | All contract rejection rules | T001-T004, T007 |

Coverage: 22/22 functional requirements mapped.

## Success Criteria

| Criterion | Evidence owner |
|---|---|
| SC-001 | T001/T004 subject manifests and preflight |
| SC-002 | T003/T004 manifest plus T005/T006 receipts |
| SC-003 | T002 event join plus T005/T006 raw reconciliation |
| SC-004 | T002/T003 schema gates plus T005-T007 summaries |
| SC-005 | T003 analyzer and T005-T007 cell/rate reports |
| SC-006 | T007 final comparison |
| SC-007 | T003 rejection tests and T007 claim audit |
| SC-008 | T003 one-attempt state machine and T005-T007 closure |
| SC-009 | T001 ancestry manifest and T007 final wording |

Coverage: 9/9 success criteria mapped.

## User Stories

| Story | Independent gate | Tasks |
|---|---|---|
| US1 fair pure-PubSub benchmark | exact bases + identical Boost 1.71 patch builds + two non-formal smokes + sealed manifest | T001-T004 |
| US2 old commit first | 5 validated baseline receipts and no treatment start | T005 |
| US3 latest comparison | 5 validated treatment receipts + final guarded analysis | T006-T007 |
