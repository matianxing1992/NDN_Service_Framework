# Requirement and Gap Traceability

| Requirement | Story | Contract | Tasks | Closing evidence / success |
| --- | --- | --- | --- | --- |
| FR-001 | US6 | DB001,candidate | T001,T009 | fullentry/source/TU/target matrix;SC001/005 |
| FR-002,FR-003 | US1 | DB001,DB003 | T001,T009,T010；G002修复T003 | A01–14/nativeowner/receipts;SC003/004 |
| FR-004 | US1 | DB003 | T003 | Btimeout→Csourcefailure actualworker;SC002/003 |
| FR-005,FR-006 | US2 | DB002 | T002 | postrenamefaults/oldinode/absence/recovery/cleanup;SC002 |
| FR-007 | US2 | DB002,DB005,DB006 | T002,T005,T006 | identity/location/deadline/dedup/cooldown;SC003/004 |
| FR-008 | US3 | DB005 | T005 | A19artifact/O07segment/A20offline separate;SC004 |
| FR-009,FR-010 | US3 | DB005 | T005 | allO17subscenarios/originalwire/authplanes;SC004 |
| FR-011 | US4 | DB007 | T007 | O/L/N methods/registerednetwork/local/codec;SC001/004 |
| FR-012 | US4 | DB004 | T004 | actualimport parent-fsync/replay/sourceclosure;SC002/004 |
| FR-013,FR-014 | US5 | DB007 | T007,T008 | nativebinding/DI/P01–06/rejectionzeroeffects;SC004 |
| FR-015 | US6 | DB001,DB006,candidate | T001,T006,T010 | all59IDs/subscenarios/newregressions;SC001/006 |
| FR-016,FR-017 | US6 | candidate | T009 | fullaffected/dynamic/mutation/freshaudit;SC005 |
| FR-018 | US6 | candidate | T010 | samefamilytworounds/crossrun/pcap/child/cleanup;SC006 |
| FR-019 | US6 | DB001,supersession | T001,T009,T010 | allpredecessorintent+gapowners retained;SC001/006 |

G001→T002，G002→T003，G003→T004，G004→T001/T009，G005→T009/T010，G006→T005，G007→T006，G008→T005/T010，G009→T001/T007/T009，G010→operationalpointer/index+T001baseline。每gap的源码/严重程度见gap-audit；不把evidencegap转换成没有演示依据的productdefect。

每个任务都有FR/contract/oracle；没有仅为task数量添加的任务。SC001由清单与T010零OPEN审计，SC002由三个repair真实nativefault闭合，SC003由worker/receipts不可变，SC004由正确安全/模式/native边界，SC005由sealedsource与fresh门，SC006由完整tworounds。任何component/static记录不自动勾parent。
