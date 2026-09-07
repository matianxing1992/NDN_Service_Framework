# Traceability

| Requirement | Story | Contract/decision | Tasks | Validation | Success |
| --- | --- | --- | --- | --- | --- |
| FR-001 | US1/US4 | canonical ownership | T001,T003,T004,T017 | V19 | SC-006 |
| FR-002 | US1 | single strict profile | T002,T004 | V01,V03 | SC-001,SC-004 |
| FR-003 | US1 | I/R/E and invalidation | T001,T002,T007 | V02,V07 | SC-001,SC-004 |
| FR-004 | US1 | stage-specific zero-side-effects | T002,T004 | V01,V02,V03 | SC-001 |
| FR-005 | US2 | dependency/build closure | T001,T008,T011 | V08,V12 | SC-002 |
| FR-006 | US2/US3 | local SIF / compute version | T011,T012 | V12,V13,V14 | SC-002,SC-003 |
| FR-007 | US2/US3 | identity/real readiness | T003,T005,T009,T010,T012 | V03,V05,V09,V10,V11,V14 | SC-002,SC-005 |
| FR-008 | US3 | DAG and placement | T005,T013,T014 | V05,V15,V16 | SC-003 |
| FR-009 | US2/US3 | NDN dependency path | T005,T009,T010,T014 | V09,V11,V16 | SC-002,SC-003 |
| FR-010 | US2/US3 | numerical contract | T005,T006,T009,T014 | V06,V09,V16 | SC-002,SC-003 |
| FR-011 | US2/US3 | terminal agreement | T003,T006,T013,T014 | V04,V06,V15,V16 | SC-003,SC-005 |
| FR-012 | US1/US4 | allocation/cleanup ownership | T003,T004,T015 | V03,V04,V17 | SC-005 |
| FR-013 | US4 | independent allocation repetition | T016 | V18 | SC-003,SC-004 |
| FR-014 | all | audit and gate order | T007,T008,T009,T010,T011,T012,T013,T014 | V07–V16 | SC-002,SC-003 |
| FR-015 | all | exact negative boundary | T002,T003,T004,T005,T006,T009,T010,T015 | V01–V06,V10,V11,V17 | SC-001,SC-005 |
| FR-016 | US4 | receipts and immutable results | T006,T014,T015,T016,T017 | V06,V16,V17,V18,V19 | SC-004,SC-005,SC-006 |
| FR-017 | US1/US4 | capacity/CAS | T001,T002,T011,T012,T016 | V02,V12,V14,V18 | SC-004,SC-006 |
| FR-018 | US4 | one operator interface | T004,T017 | V01,V19 | SC-006 |

每个任务至少映射一个实际需求；17 项为可审查行为，不代表17个文件。此表为 planned coverage，不宣称运行覆盖已达成。
