# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 3 | **Status**: DRAFT / NOT_STARTED
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Current Checkpoint

2026-09-06 revision 3：依据用户最新要求先强化设计写作规则，再按合并工作区源码修正182。增加类/方法/字段/注释/用法契约，修正合并基线、Core安全继承及 CandidateBudget。实现进度 **0/17**；本轮未运行build、unit/integration/MiniNDN或外部实验。

当前182为活动设计；不再要求先完成181全部旧资格。合并工作区尚有未提交修复，记录中的 unit R1 747/751、integration R1 60/92 不能称PASS。本轮只读取并区分这些已有失败，不修改其修复代码或索引。合并 closure、嵌套schema/兼容清单、原生ABI和隔离设计仍由 O-001--005 控制；本文不勾选T001。

本轮文档检查与来源身份见 [revision 3 evidence](evidence/skill-and-design-revision3.md)；旧 authoring/audit 记录保留历史身份。下一步在合并修复稳定后执行T001，按新契约关闭具体未决项，再进入T002原生库实现。

## Validation Standard

每个行为任务包含 unit test、integration、adjacent regression 和 proof-design 指定的 validation。
正式 MiniNDN 必须在 T015 PASS 后执行；本文只规定计划，不运行上述检查。

## Phase 1: Design and Native Components

- [ ] T001 [US5] **Successor Baseline and Design Closure**。冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。Dependencies: Merged baseline closure and Spec181 handoff。
  Design: FR-015,FR-016,FR-017; CD-001--014。Proof: PO-012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [ ] T002 [US1] **Installable Native Library Contract**。existing Provider runtime 可独立安装/链接；planned requester 公开声明先冻结，完整 request 实现由 T010 验收。Dependencies: T001。
  Design: FR-001,FR-012; CD-001,CD-009。Proof: PO-001。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T002 contract](contracts/work-units.md#t002-installable-native-library-contract)。

- [ ] T003 [US2] **Native Split and Placement Decisions**。两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。Dependencies: T002。
  Design: FR-003,FR-009,FR-016; CD-002。Proof: PO-002。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T003 contract](contracts/work-units.md#t003-native-split-and-placement-decisions)。

- [ ] T004 [US1] **Canonical Native Plan Sealing**。合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。Dependencies: T003。
  Design: FR-002,FR-004; CD-003。Proof: PO-003。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T004 contract](contracts/work-units.md#t004-canonical-native-plan-sealing)。

- [ ] T005 [US1] **Native Requester Grant Path**。原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。Dependencies: T004。
  Design: FR-005; CD-004。Proof: PO-004。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T005 contract](contracts/work-units.md#t005-native-requester-grant-path)。

- [ ] T006 [US1] **Native Cold ONNX Assembly**。Selection 后原生装配与既有固定 bytes 一致，消除生产 helper IPC。Dependencies: T002；O-002 closed。
  Design: FR-006,FR-016; CD-005。Proof: PO-005。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T006 contract](contracts/work-units.md#t006-native-cold-onnx-assembly)。

- [ ] T007 [US4] **Native Tokenizer Execution**。原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。Dependencies: T002；O-003 closed。
  Design: FR-007; CD-006。Proof: PO-006。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T007 contract](contracts/work-units.md#t007-native-tokenizer-execution)。

- [ ] T008 [US1] **Native Request Preparation and Admission**。原生输入/认证模型/工件准备与 offer policy 校验闭合，GraphAdapter/TaskAdapter 端口由 native 实现。Dependencies: T003/T006/T007。
  Design: FR-001,FR-002,FR-004,FR-009,FR-016; CD-013。Proof: PO-013。
  [T008 contract](contracts/work-units.md#t008-native-request-preparation-and-admission)。

- [ ] T009 [US3] **Shared Native Provider Host**。CLI/C++/Python 共用服务注册、准备/执行接线与停止语义。Dependencies: T006/T007。
  Design: FR-001,FR-009,FR-010,FR-012; CD-014。Proof: PO-014。
  [T009 contract](contracts/work-units.md#t009-shared-native-provider-host)。

## Phase 2: Invocation and Compatibility

- [ ] T010 [US1] **Complete Native Request Lifecycle**。独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。Dependencies: T003/T004/T005/T006/T007/T008/T009。
  Design: FR-001,FR-002,FR-008; CD-001,CD-013,CD-014。Proof: PO-001,PO-003,PO-007,PO-013,PO-014。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T010 contract](contracts/work-units.md#t010-complete-native-request-lifecycle)。

- [ ] T011 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T010。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T011 contract](contracts/work-units.md#t011-native-conversation-continuation)。

- [ ] T012 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T011。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T012 contract](contracts/work-units.md#t012-thin-python-native-bindings)。

- [ ] T013 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T012。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T013 contract](contracts/work-units.md#t013-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T014 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路；交付正式 MiniNDN harness/collector 并完成定向自检。Dependencies: T013。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T014 contract](contracts/work-units.md#t014-runtime-dependency-exclusion-gate)。

- [ ] T015 [US5] **Design-code Convergence Audit**。逐 FR/CD/INV/PO 核对生产接线、effective config、依赖/源码身份，控制性发现清零。Dependencies: T014。
  Design: FR-013,FR-017; CD-001--014。Proof: PO-001--014。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T015 contract](contracts/work-units.md#t015-design-code-convergence-audit)。

- [ ] T016 [US5] **Local Native Qualification**。同源完整 suites、YOLO/Qwen MiniNDN 和 no-Python 全部通过。Dependencies: T015 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--014。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T016 contract](contracts/work-units.md#t016-local-native-qualification)。

- [ ] T017 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T016 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T017 contract](contracts/work-units.md#t017-native-development-handoff)。

## Dependencies & Execution Order

Merged baseline closure and Spec181 handoff → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003/T006/T007 → T008；T006/T007 → T009；
T003--009 → T010 → T011 → T012 → T013 → T014 → T015 PASS → T016 → T017。
只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。
