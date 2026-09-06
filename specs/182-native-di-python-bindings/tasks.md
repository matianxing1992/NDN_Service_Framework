# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 1 | **Status**: DRAFT / NOT_STARTED
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Current Checkpoint

2026-09-06：仅创建后续 Spec182 设计。Spec181 继续活动；feature 指针与其任务状态不变。
本轮交付 spec/plan/CD/INV/PO/work units/traceability/checklist/audit/baseline；
实现进度 **0/15**，未启动功能迁移、build、unit/integration/MiniNDN、SIF 或 Tiger。
文档检查结果与工具 fallback 见 [design review](evidence/design-review.md)。
首次提交被路径引用钩子拒绝，清理非产品引用后结构审计再次 PASS；实现验收仍未执行。
设计状态 DRAFT：O-001--004 控制 implementation readiness，O-005 控制 native isolation gate。
文档创建完成不勾选实现任务。下一步继续 Spec181；关闭后再执行 T001。

## Validation Standard

每个行为任务包含 unit test、integration、adjacent regression 和 proof-design 指定的 validation。
正式 MiniNDN 必须在 T013 PASS 后执行；本文只规定计划，不运行上述检查。

## Phase 1: Design and Native Components

- [ ] T001 [US5] **Successor Baseline and Design Closure**。冻结 181 交付、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--004；修订叶子签名与任务至可执行。Dependencies: Spec181 local closure。
  Design: FR-015,FR-016; CD-001--012。Proof: PO-012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [ ] T002 [US1] **Installable Native Library Contract**。原生库可安装，独立 consumer 可链接已冻结类型和生命周期接口，无 Python 链接依赖。Dependencies: T001。
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

## Phase 2: Invocation and Compatibility

- [ ] T008 [US1] **Complete Native Request Lifecycle**。独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。Dependencies: T003/T004/T005/T006/T007。
  Design: FR-001,FR-002,FR-008; CD-001。Proof: PO-001,PO-003,PO-007。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T008 contract](contracts/work-units.md#t008-complete-native-request-lifecycle)。

- [ ] T009 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T008。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T009 contract](contracts/work-units.md#t009-native-conversation-continuation)。

- [ ] T010 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T009。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T010 contract](contracts/work-units.md#t010-thin-python-native-bindings)。

- [ ] T011 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T010。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T011 contract](contracts/work-units.md#t011-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T012 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路。Dependencies: T011。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T012 contract](contracts/work-units.md#t012-runtime-dependency-exclusion-gate)。

- [ ] T013 [US5] **Design-code Convergence Audit**。逐 FR/CD/INV/PO 核对生产接线、effective config、依赖/源码身份，控制性发现清零。Dependencies: T012。
  Design: FR-013; CD-001--012。Proof: PO-001--012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T013 contract](contracts/work-units.md#t013-design-code-convergence-audit)。

- [ ] T014 [US5] **Local Native Qualification**。同源完整 suites、YOLO/Qwen MiniNDN 和 no-Python 全部通过。Dependencies: T013 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--011。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T014 contract](contracts/work-units.md#t014-local-native-qualification)。

- [ ] T015 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T014 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  完整文件/符号、decision budget、预期 diff、命令、恢复点与证据：
  [T015 contract](contracts/work-units.md#t015-native-development-handoff)。

## Dependencies & Execution Order

Spec181 closure → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003--007 → T008 → T009 → T010 → T011 → T012 → T013 PASS → T014 → T015。
只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。
