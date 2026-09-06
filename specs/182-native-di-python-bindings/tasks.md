# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 6 | **Status**: DRAFT / NOT_STARTED
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Current Checkpoint

2026-09-06 revision 6：合并重复审查与报告；实现任务只做静态审查、相关unit及必要构建，集成与MiniNDN在全部实现后由T016统一运行。类/方法/字段设计和既定真实运行用例保留。

本轮只改技能与文档；实现 **0/17**，产品STATIC_REVIEW与unit/integration/MiniNDN均 **NOT_RUN**。
文档结构、107链接、依赖及技能引用检查PASS；既定PO-001--014/负例/运行用例与符号字段表的保留检查PASS。范围见 [workflow simplification](evidence/workflow-simplification.md)。
O-001--005和合并修复状态未被本轮关闭。下一步执行T001确认基线与设计就绪。

## Validation Standard

唯一规则见 [validation workflow](contracts/pre-test-static-review.md)。
T002--T014的任务[x]仅代表本任务实现、静态审查、相关单测和必要构建完成；
其Proof行引用完整行为义务，跨组件/跨进程与MiniNDN证明登记给T016，不要求各任务提前运行。
T015在全部实现后补审整体接线；T016执行完整unit→integration→MiniNDN并关闭全部必需PO。
不得将真实集成重命名为unit/smoke提前执行。Static review PASS != Behavior PASS。
每任务只保留简短结果或一份evidence链接，最终核对diff与证据，不另建S0/S1报告。

## Phase 1: Design and Native Components

- [ ] T001 [US5] **Successor Baseline and Design Closure**。冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。Dependencies: Merged baseline closure and Spec181 handoff。
  Design: FR-015,FR-016,FR-017,FR-018; CD-001--014。Proof: PO-012。
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [ ] T002 [US1] **Installable Native Library Contract**。existing Provider runtime 可独立安装/链接；planned requester 公开声明先冻结，完整request由T010实现、T016运行验收。Dependencies: T001。
  Design: FR-001,FR-012; CD-001,CD-009。Proof: PO-001。
  [T002 contract](contracts/work-units.md#t002-installable-native-library-contract)。

- [ ] T003 [US2] **Native Split and Placement Decisions**。两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。Dependencies: T002。
  Design: FR-003,FR-009,FR-016; CD-002。Proof: PO-002。
  [T003 contract](contracts/work-units.md#t003-native-split-and-placement-decisions)。

- [ ] T004 [US1] **Canonical Native Plan Sealing**。合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。Dependencies: T003。
  Design: FR-002,FR-004; CD-003。Proof: PO-003。
  [T004 contract](contracts/work-units.md#t004-canonical-native-plan-sealing)。

- [ ] T005 [US1] **Native Requester Grant Path**。原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。Dependencies: T004。
  Design: FR-005; CD-004。Proof: PO-004。
  [T005 contract](contracts/work-units.md#t005-native-requester-grant-path)。

- [ ] T006 [US1] **Native Cold ONNX Assembly**。Selection 后原生装配与既有固定 bytes 一致，消除生产 helper IPC。Dependencies: T002；O-002 closed。
  Design: FR-006,FR-016; CD-005。Proof: PO-005。
  [T006 contract](contracts/work-units.md#t006-native-cold-onnx-assembly)。

- [ ] T007 [US4] **Native Tokenizer Execution**。原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。Dependencies: T002；O-003 closed。
  Design: FR-007; CD-006。Proof: PO-006。
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
  [T010 contract](contracts/work-units.md#t010-complete-native-request-lifecycle)。

- [ ] T011 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T010。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  [T011 contract](contracts/work-units.md#t011-native-conversation-continuation)。

- [ ] T012 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T011。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  [T012 contract](contracts/work-units.md#t012-thin-python-native-bindings)。

- [ ] T013 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T012。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  [T013 contract](contracts/work-units.md#t013-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T014 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路；交付正式 MiniNDN harness/collector 并完成本地单测；真实隔离反例在T016运行。Dependencies: T013。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  [T014 contract](contracts/work-units.md#t014-runtime-dependency-exclusion-gate)。

- [ ] T015 [US5] **Design-code Convergence Audit**。整体静态读码核对FR/CD/INV/PO、生产接线、test/oracle/harness与依赖身份，控制性发现清零；记录整体审查结论并进入T016。Dependencies: T014。
  Design: FR-013,FR-017,FR-018; CD-001--014。Proof: PO-001--016。
  [T015 contract](contracts/work-units.md#t015-design-code-convergence-audit)。

- [ ] T016 [US5] **Local Native Qualification**。全部实现和T015审查完成后，同源完整unit→integration→YOLO/Qwen MiniNDN/no-Python及必要检错证明通过，核对最终diff与证据。Dependencies: T015 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--016。
  [T016 contract](contracts/work-units.md#t016-local-native-qualification)。

- [ ] T017 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T016 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  [T017 contract](contracts/work-units.md#t017-native-development-handoff)。

## Dependencies & Execution Order

Merged baseline closure and Spec181 handoff → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003/T006/T007 → T008；T006/T007 → T009；
T003--009 → T010 → T011 → T012 → T013 → T014 → T015 PASS → T016 → T017。
所有任务遵循FR-018/019和统一验证规则；每任务具体范围见work-units。只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。
