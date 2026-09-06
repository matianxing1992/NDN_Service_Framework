# Spec182 Traceability Matrix

**Revision**: 2 | **Status**: DRAFT / NOT_STARTED
source review 只证明 baseline 所述实现存在，不证明新实现或资格。
所有 task completion evidence 路径为 planned，revision 2 文档检查见 evidence/audit-revision2.md；revision 1 历史记录不改写。

## Requirement-to-task Map

| Requirement | Success criteria | Design | Tasks | Proof obligations | Flow |
| --- | --- | --- | --- | --- | --- |
| FR-001 | SC-001,SC-007 | CD-001,CD-009,CD-011,CD-013,CD-014 | T002,T008,T009,T010,T014,T016 | PO-001,PO-011,PO-013,PO-014 | FLOW-001 |
| FR-002 | SC-002,SC-003 | CD-001,CD-003,CD-013 | T004,T008,T010 | PO-003,PO-013 | FLOW-001 |
| FR-003 | SC-002,SC-003 | CD-002 | T003,T012 | PO-002,PO-009 | FLOW-001 |
| FR-004 | SC-002,SC-003 | CD-003,CD-013 | T004,T008 | PO-003,PO-013 | FLOW-001 |
| FR-005 | SC-004,SC-005 | CD-004 | T005,T016 | PO-004,PO-011 | FLOW-001 |
| FR-006 | SC-001,SC-004 | CD-005 | T006,T016 | PO-005,PO-011 | FLOW-001 |
| FR-007 | SC-001,SC-005 | CD-006 | T007,T016 | PO-006,PO-011 | FLOW-003 |
| FR-008 | SC-002,SC-005 | CD-001,CD-007 | T010,T011,T016 | PO-007,PO-008,PO-011 | FLOW-002,FLOW-003 |
| FR-009 | SC-003,SC-005 | CD-002,CD-012,CD-013,CD-014 | T003,T008,T009,T015,T017 | PO-002,PO-011,PO-013,PO-014 | FLOW-001 |
| FR-010 | SC-002,SC-006 | CD-008,CD-014 | T009,T012,T016 | PO-009,PO-014 | FLOW-004 |
| FR-011 | SC-006 | CD-010,CD-011 | T013,T014,T016 | PO-010 | FLOW-004 |
| FR-012 | SC-001,SC-007 | CD-009,CD-011,CD-014 | T002,T009,T014,T016 | PO-001,PO-012,PO-014 | FLOW-001 |
| FR-013 | SC-004,SC-005,SC-008 | CD-011 | T015,T016 | PO-001--014 | FLOW-001--004 |
| FR-014 | SC-007,SC-008 | CD-009,CD-012 | T014,T016,T017 | PO-012 | FLOW-001 |
| FR-015 | SC-008 | CD-012 | T001,T017 | PO-012 | FLOW-001 |
| FR-016 | SC-004,SC-005,SC-006 | CD-002,CD-005,CD-007,CD-010,CD-013 | T001,T003,T006,T008,T011,T013,T016 | PO-002,PO-005,PO-008,PO-010,PO-013 | FLOW-001,FLOW-003 |

## Design-to-task Map

| CD | Tasks | Exact source / test / evidence |
| --- | --- | --- |
| CD-001 | T002,T010 | contracts/code-design.md#cd-001-public-api；proof-design PO-001/007 |
| CD-002 | T003 | contracts/code-design.md#cd-002-strategies；PO-002 |
| CD-003 | T004 | contracts/code-design.md#cd-003-plan-semantics；PO-003 |
| CD-004 | T005 | contracts/code-design.md#cd-004-grant；PO-004 |
| CD-005 | T006 | contracts/code-design.md#cd-005-assembly；PO-005 |
| CD-006 | T007 | contracts/code-design.md#cd-006-tokenizer；PO-006 |
| CD-007 | T011 | contracts/code-design.md#cd-007-lifecycle；PO-007/008 |
| CD-008 | T012 | contracts/code-design.md#cd-008-bindings；PO-009 |
| CD-009 | T002,T012 | contracts/code-design.md#cd-009-build；PO-001/012 |
| CD-010 | T013 | contracts/code-design.md#cd-010-migration；PO-010 |
| CD-011 | T014,T015,T016 | contracts/proof-design.md；PO-001--014 |
| CD-012 | T001,T017 | plan.md#delivery；PO-012 |
| CD-013 | T008,T010 | contracts/runtime-boundaries.md#cd-013-preparation-and-offer-admission；PO-013 |
| CD-014 | T009,T012 | contracts/runtime-boundaries.md#cd-014-provider-host-and-binding；PO-014 |

## Readiness and Ownership

T001 关闭 OPEN 并刷新 baseline 后补足叶子签名、精确默认值、legacy callers 和 dependency lock；
本矩阵覆盖设计目标，不将其称为已可直接实施。
本机实现、unit/integration/MiniNDN 和交付；实验机器 SIF/Tiger 为 TRANSFERRED。
当前活动指针和 Spec181 任务保持原状；未来 active-Spec audit 不能用本文 DRAFT 替代。
