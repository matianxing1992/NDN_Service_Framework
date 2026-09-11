# Tasks: Native DI Closure

**Status**: PLANNED / implementation NOT_STARTED | **Date**: 2026-09-11
**Input**: [spec](spec.md)、[plan](plan.md)、[transfer matrix](contracts/transfer-matrix.md)、
[promotion candidate](contracts/promotion-candidate.md)、[caller matrix](contracts/caller-matrix.md)、
[qualification matrix](contracts/qualification-matrix.md)

## Execution Progress

| Batch | Status | Next / remaining |
| --- | --- | --- |
| B1 | NOT_STARTED | T001 authority IO dispatch，随后 T002 turn/attempt 同步；统一定向构建/测试 |
| B2 | NOT_STARTED | T003 durable commit 与终态 |
| B3 | NOT_STARTED | T004 安全 checkpoint 导出 |
| B4 | NOT_STARTED | T005 当前 caller/mode 收敛 |
| B5 | NOT_STARTED | T006 矩阵/缺口修复 → fresh convergence audit `PASS` → T007 正式本地资格 → T008 交付 |

## Current Checkpoint

2026-09-11 **D-UAV-TRIM / CLOSED_FOR_VALIDATION (documentation only)**：按用户要求删除
UAV update PDF 原第4/5页，现4页；双遍构建及全页渲染通过，两份导出同步。
[证据](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md)。不推进 B1–B5，原生下一步保持不变。

2026-09-11 **D-UAV-CASE / CLOSED_FOR_VALIDATION (documentation only)**：
[UAV update PDF](../../docs/NDNSF-UAV/slides/UPDATES_UAV.pdf) 已按案例价值审查修订，6页双遍构建、
文本密度与渲染检查通过；[证据](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md)。
无产品/API/实验变化，不推进本 Spec 的 B1–B5；下一原生工作仍按下述文档门禁复核后进入 B1/T001。

文档移交和执行门禁已建立；产品实现0个任务完成，下一步为文档门禁复核后进入 B1/T001。
Spec182 的14个 OPEN 父任务和 R12-A–E 全部承接；未搬运历史长记录，既有 PASS 只保留原证据范围。
本轮只修改 Spec184/历史指针文档，未改产品代码、未编译/运行产品测试；迁移文档的结构 PASS
不授权产品实现或 qualification。迁移记录见 [migration record](evidence/migration-20260911.md)，本轮门禁收口见
[documentation gate repair](evidence/documentation-gate-repair-20260911.md)。

## Phase 1: Request Correctness

- [ ] T001 [US1] **Authority IO Dispatch**. FR-001；修复 F-01，在 `NativeAuthenticatedGrantClient::coreIssue` 将 `ServiceUser::RequestServiceTargeted` 封送到 Core `postToIo`，处理 dispatch 前取消、空 request ID、异常、timeout 与晚回调；在 `tests/integration-tests/di-native-requester-grant.t.cpp` 的 `Spec184AuthorityIoOwnership` 中验证线程 owner、真实 `ServiceUser` 状态和 bounded cleanup。Target: `integration-tests`（已由 `tests/wscript` 注册 TU）。Dependencies: documentation gate and migration baseline。
- [ ] T002 [US1] **Turn Publication Synchronization**. FR-002；修复 F-02，核对 `NativeInferenceClient` turn/attempt mutable-state、publication linearization 和 ticket 清理；在 `tests/unit-tests/di-native-client.t.cpp` 的 `Spec184TurnPublicationRace` 中覆盖 cancel/deadline/close/replacement。Target: `unit-tests`（unit glob 注册）。与 T001 组成 B1，逐任务静态门后统一运行交错用例。Dependencies: T001 static gate。
- [ ] T003 [US1] **Durable Outcome Linearization**. FR-003；修复 F-03，client/coordinator publish 与 handle outcome 一致；在 `tests/integration-tests/ndnsf-di-core-flow.t.cpp` 的 `Spec184DurableOutcome` 中覆盖 publish 后阻塞 FINALIZE、取消和超时。Target: `integration-tests`。Dependencies: B1 behavior exit。

## Phase 2: Export and Callers

- [ ] T004 [US2] **Atomic Private Checkpoint Export**. FR-004；修复 F-04，`DI_NativeRequester` 以同目录临时文件、`0600`、write/`fsync`/close/rename/目录 `fsync` 安全导出，明确 symlink 不跟随、失败保留和 round-trip；补 `tests/unit-tests/di-native-checkpoint.t.cpp` 的 `Spec184CheckpointExport`。Target: `unit-tests`。Dependencies: B2 exit。
- [ ] T005 [US2] **Maintained Caller Mode Closure**. FR-005；按 [caller matrix](contracts/caller-matrix.md) 盘点五组维护入口及其模式、默认路由、native/compatibility/removed 状态、C++ oracle、zero-use 和 rollback；按共享逻辑分组迁移并检查薄绑定。Dependencies: B3 exit。

## Phase 3: Qualification and Delivery

- [ ] T006 [US3] **Inherited Obligation and Harness Closure**. FR-006；维护 [qualification matrix](contracts/qualification-matrix.md)，逐项对账原14个 OPEN 任务、PO-001–016及适用 I/FR/CD/INV，并分别给出 obligation、component、harness、identity 和 external-owner 行；补残余组件/装配/tokenizer/host/绑定实现或 fixture、trace/marker/build identity 缺口。每个新增实现子组先静态门再定向 C++ 验证，候选冻结且控制性 finding 清零后运行 fresh convergence audit。Dependencies: B4 exit；只读对账可提前。
- [ ] T007 [US3] **Current Native Qualification**. FR-006；仅在 T006 的 qualification matrix 完整且 `evidence/convergence-b5.md` 为当前 candidate 的 fresh `PASS` 后，按矩阵运行同源完整 unit/integration、YOLO/Qwen MiniNDN/no-Python 与检错负例；绑定源码/二进制/日志，区分局部 PASS 和正式 qualification；不重跑未受影响的历史实验。Dependencies: T006 static/focused exits and fresh convergence `PASS`。
- [ ] T008 [US3] **Native Development Handoff**. FR-006；同步 Design/API/使用说明、两个入口示例、最终源码基线、剩余外部实验 TRANSFERRED 状态；不得以文档移交替代本地资格。Dependencies: T007 PASS。

## Dependencies & Execution Order

B1(T001/T002) → B2(T003) → B3(T004) → B4(T005) → B5(T006–T008)。
所有 T ID 属于184；引用182时必须加 Spec 前缀（例如 `182:T005`）避免歧义。
T005/T006 的子组在所属矩阵维护，不继续在 tasks.md 堆积上百个 G 编号。
静态审查通过但批次 C++ 验证未通过时保持 `[ ]` / PARTIAL；每批只维护一个结果记录。
任务状态不能由 migration record、文档结构 PASS、CLI smoke 或 Python wrapper 结果提升；
T007 也不能替代 T001–T006 的定向 C++ 出口。

## Validation

每个任务的 C++ test/negative oracle 见 spec Acceptance Evidence Contract 和注册表；批次组合
审查通过后统一运行相关验证。T007 是最终 qualification，不替代前置批次定向 test，且必须
服从 [promotion candidate](contracts/promotion-candidate.md) 的失效矩阵。
