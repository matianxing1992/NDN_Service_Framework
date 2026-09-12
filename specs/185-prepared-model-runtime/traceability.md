# Traceability

[C-08](contracts/code-design.md#design-binding-and-readiness)及[C-09](contracts/core-app-boundary.md)覆盖18任务→类/字段/函数/流程/证明；通用owner修订由C-09约束，公开A01–A64沿C-07复用。

| Requirement | Story | Audit / contract | Tasks | Success / selector |
| --- | --- | --- | --- | --- |
| FR-013 | US1,US2 | C-09 CB01–CB04 | T017,T018,T001,T002,T004,T006,T009 | SC-008 / Spec185CoreOperation, Spec185DiCoreOperation |
| FR-001 | US1 | A08 / C-01 | T001,T002 | SC-004 / Spec185Runtime |
| FR-002 | US1 | A01,A02 / C-02 | T003 | SC-001,SC-003 / Spec185Preparation |
| FR-003 | US1 | A03 / C-02 | T004 | SC-001,SC-002 / Spec185Preparation |
| FR-004 | US2 | A04,A05 / C-03 | T005 | SC-001,SC-003 / Spec185PreparedRequest |
| FR-005 | US2 | A08,A09,A10 / C-01 | T006 | SC-004 / Spec185PreparedRequest |
| FR-006 | US2 | A08 / C-03 | T007,T008 | SC-004 / Spec185Conversation |
| FR-007 | US3 | A06,A07 / C-03 | T009,T010 | SC-003,SC-004 / Spec185ProviderAssembly |
| FR-008 | US4 | A10,A11 / C-04 | T011,T012 | SC-005 / Spec185Compatibility |
| FR-009 | all | A11 / C-04 | T013,T014 | SC-001–SC-005 / Spec185Process + Design checks |

每个selector均PLANNED/NOT_RUN；真实结果由tasks顶部Execution Progress链接到同批evidence。

## API Revision Trace

| Requirement | Story | Audit / contract | Tasks | Success / selector |
| --- | --- | --- | --- | --- |
| FR-010 | US4 | U10,U11,U12 / C-05,C-06 | T015 | SC-006 / Spec185InstalledApi |
| FR-011 | US4 | U13,U14,U15 / C-05 | T016 | SC-007 / Spec185ExtensionRegistry |
| FR-012 | US1–US4 | U04–U09 / C-05,C-06 | T003,T006,T009,T011,T013,T012 | SC-006,SC-007 / Spec185Process then wrappers |

U编号见[API审计](api-review.md)；T013先验SC-005原生行，T012再关闭包装行。

FR-001/FR-005/FR-007/FR-008/FR-012同时追踪[C-07 A01–A64及生命周期矩阵](contracts/api-catalog.md)，任务归属逐行列明；SC-004/SC-006/SC-007只有对应C++行为和安装消费完整通过才关闭。
