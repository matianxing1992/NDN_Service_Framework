# Tasks: Native DI Closure

**Status**: PLANNED / implementation NOT_STARTED | **Date**: 2026-09-11
**Input**: [spec](spec.md)、[plan](plan.md)、[transfer matrix](contracts/transfer-matrix.md)

## Execution Progress

| Batch | Status | Next / remaining |
| --- | --- | --- |
| B1 | NOT_STARTED | T001 authority IO dispatch，随后 T002 turn/attempt 同步；统一定向构建/测试 |
| B2 | NOT_STARTED | T003 durable commit 与终态 |
| B3 | NOT_STARTED | T004 安全 checkpoint 导出 |
| B4 | NOT_STARTED | T005 当前 caller/mode 收敛 |
| B5 | NOT_STARTED | T006 全项对账/缺口修复 → T007 正式本地资格 → T008 交付 |

## Current Checkpoint

文档移交已建立；产品实现0个任务完成，下一步 B1/T001。Spec182 的14个 OPEN 父任务和
R12-A–E 全部承接；未搬运历史长记录，既有 PASS 只保留原证据范围。
本次未改产品、未编译/运行产品测试；文档验证见 [migration record](evidence/migration-20260911.md)。

## Phase 1: Request Correctness

- [ ] T001 [US1] **Authority IO Dispatch**. FR-001；修复 F-01，在 NativeAuthenticatedGrantClient.cpp 封送提交并处理取消/异常/晚回调；补 spec 所列 C++ fixture。Dependencies: migration baseline。
- [ ] T002 [US1] **Turn Publication Synchronization**. FR-002；修复 F-02，核对 NativeInferenceClient.cpp mutable-state 读写与 ticket 清理；与 T001 组成 B1，逐任务静态门后统一运行交错用例。Dependencies: T001 static gate。
- [ ] T003 [US1] **Durable Outcome Linearization**. FR-003；修复 F-03，client/coordinator publish 与 handle outcome 一致；在 publish 后阻塞 FINALIZE 的 C++ fixture 中测试取消/超时。Dependencies: B1 behavior exit。

## Phase 2: Export and Callers

- [ ] T004 [US2] **Atomic Private Checkpoint Export**. FR-004；修复 F-04，DI_NativeRequester 安全导出及失败保留/round-trip 测试。Dependencies: B2 exit。
- [ ] T005 [US2] **Maintained Caller Mode Closure**. FR-005；生成 contracts/caller-matrix.md，盘点五组维护入口及其模式、默认路由、native/compatibility/removed 状态与 C++ oracle；按共享逻辑分组迁移并检查薄绑定。Dependencies: B3 exit。

## Phase 3: Qualification and Delivery

- [ ] T006 [US3] **Inherited Obligation and Harness Closure**. FR-006；生成 contracts/qualification-matrix.md，逐项对账原14个 OPEN 任务、PO-001–016及适用 I/FR/CD/INV；补残余组件/装配/tokenizer/host/绑定实现或 fixture、trace/marker/build identity 缺口。每个新增实现子组先静态门再定向 C++ 验证，整体控制性 finding 清零后进入 T007。Dependencies: B4 exit；只读对账可提前。
- [ ] T007 [US3] **Current Native Qualification**. FR-006；按 T006 当前矩阵运行同源完整 unit/integration、YOLO/Qwen MiniNDN/no-Python 与检错负例；绑定源码/二进制/日志，区分局部 PASS 和正式 qualification；不重跑未受影响的历史实验。Dependencies: T006 static and focused exits。
- [ ] T008 [US3] **Native Development Handoff**. FR-006；同步 Design/API/使用说明、两个入口示例、最终源码基线、剩余外部实验 TRANSFERRED 状态；不得以文档移交替代本地资格。Dependencies: T007 PASS。

## Dependencies & Execution Order

B1(T001/T002) → B2(T003) → B3(T004) → B4(T005) → B5(T006–T008)。
所有 T ID 属于183；引用182时必须加 Spec 前缀（例如 `182:T005`）避免歧义。
T005/T006 的子组在所属矩阵维护，不继续在 tasks.md 堆积上百个 G 编号。
静态审查通过但批次 C++ 验证未通过时保持 `[ ]` / PARTIAL；每批只维护一个结果记录。

## Validation

每个任务的 C++ test/negative oracle 见 spec Acceptance Evidence Contract；批次组合审查通过后
统一运行相关验证。T007 是最终 qualification，不替代前置批次定向 test。
