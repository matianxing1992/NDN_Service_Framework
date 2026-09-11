# Tasks: Native DI Closure

**Status**: IN_PROGRESS / B4 closed for validation; formal qualification pending | **Date**: 2026-09-11
**Input**: [spec](spec.md)、[plan](plan.md)、[transfer matrix](contracts/transfer-matrix.md)、
[promotion candidate](contracts/promotion-candidate.md)、[caller matrix](contracts/caller-matrix.md)、
[qualification matrix](contracts/qualification-matrix.md)

## Execution Progress

| Batch | Status | Dynamic profile / stable exit | Next / remaining |
| --- | --- | --- | --- |
| B1 | DYNAMIC_PASS / CLOSED_FOR_VALIDATION | `tsan` PASS；IO owner、turn/ticket 线性化、无 pending residue | T001/T002 已完成普通 C++ selector、独立 TSan 各两次重复；进入 B2/T003 |
| B2 | DYNAMIC_PASS / CLOSED_FOR_VALIDATION | `asan-ubsan` PASS；durable handle/journal 一致、publish 后终态不降级、residue=0 | T003 已完成；进入 B3/T004 |
| B3 | DYNAMIC_PASS / CLOSED_FOR_VALIDATION | `asan-ubsan` PASS；原子 export、symlink refusal、pre-rename failure preservation、loader smoke | T004 已完成；进入 B4/T005 |
| B4 | CLOSED_FOR_VALIDATION / PARTIAL | caller rows use `none` with reason; inherited B1/B2 profiles cover shared async owners | T005 matrix/route closure complete；D2b runtime miss、real model/no-Python and retirement remain T006/T007 |
| B5 | IN_PROGRESS / T006 PARTIAL | inherited row profiles；文档对账 `none` with reason | qualification matrix 已逐项绑定；仍需 exact current selectors/artifact/config identity、缺口修复 → fresh convergence audit `PASS` → T007 正式本地资格 → T008 交付 |

## Current Checkpoint

2026-09-11 **B1-TSAN / DYNAMIC_PASS**：B1 普通 `/usr/bin/g++` 构建完成，三个命名 C++ selector
通过；独立 `/usr/bin/clang++` TSan 构建完成，三个 selector 各重复两次，均 exit code 0 且无
ThreadSanitizer 报告。原始输出见 [B1 evidence](evidence/b1-request-correctness-20260911.md)；
TSan 仅覆盖 B1 登记的不变量，不提升后续 durable outcome、checkpoint、caller 或最终 qualification。

2026-09-11 **DYNAMIC-GATE / DOCUMENTATION_UPDATED**：共享 `speckit-code-design`、Spec Kit
模板及 Spec184 已统一登记 `Risk class`、`Dynamic profile` 和 `Dynamic invariants`。
动态分析按批次风险触发，使用独立 ASan/UBSan、TSan 或 parser-fuzz 输出树；本次仅更新流程和
矩阵，未将动态验证记为 `DYNAMIC_PASS`，产品任务仍为0个完成。同步检查、结构审计与 Context
Mode active health 已通过；下一步继续 B1 普通 C++ selector 后再运行 TSan。

2026-09-11 **D-UAV-JOINT / CLOSED_FOR_VALIDATION (documentation only)**：UAV示例改为
区域内未知目标的多视角联合辨认，4页构建/渲染通过；
[证据](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md)。不推进 B1–B5；原生任务状态保持。

2026-09-11 **B1-STATIC / CLOSED**：T001/T002 已完成逐任务只读静态审查、普通 C++ 定向测试和
独立 TSan 动态验证；生产差异、审查边界和动态不变量记录在 [B1 evidence](evidence/b1-request-correctness-20260911.md)。

2026-09-11 **B2-FOCUSED / DYNAMIC_FAIL**：T003 的 `Spec184DurableOutcome` 在普通 C++ 构建下
通过，验证 durable publish 后并发 cancel 不会把 handle 降级，checkpoint 与 coordinator 记录一致。
严格 ASan/UBSan 在 fixture teardown 首次触发外部 `ndn-svs` `new-delete-type-mismatch`；未抑制原始
日志和两次仅用于诊断的抑制重跑均记录在 [B2 evidence](evidence/b2-durable-outcome-20260911.md)。
共享动态门规则要求先修复或隔离一致 ABI 后再写 `DYNAMIC_PASS`，因此 T003 保持 `[ ]` / `PARTIAL`，
不能进入 B3。

2026-09-11 **DYNAMIC-CARD / DOCUMENTATION_UPDATED**：共享 skill、Spec Kit 模板、Spec184
`plan.md`/`spec.md` 已加入批次级 `Dynamic gate card`，冻结参数边界、C++ selector、业务不变量、
预算、toolchain/source identity、输出路径和失败分类；sanitizer 抑制不得直接升级为 `DYNAMIC_PASS`。

2026-09-11 **B2-ASAN / DYNAMIC_PASS**：先保留外部 NDN-SVS 旧头文件/库失配导致的未抑制
`new-delete-type-mismatch`，随后用当前源码重建 NDN-SVS 并重链独立 ASan/UBSan tree。普通
selector 与无抑制 sanitizer selector 各通过，sanitizer selector 共三次、均 exit code 0、无
ASan/UBSan 报告；B2 evidence 记录原始失败、依赖重建和 binary digest。T003 完成，下一步 B3/T004。

2026-09-11 **B3-ASAN / DYNAMIC_PASS**：T004 使用原生 `NativeCheckpointExport` 完成同目录临时文件、
`0600`、canonical JSON、file/directory `fsync`、原子 rename 和 symlink 拒绝。普通 C++ selector
三例通过；独立无抑制 ASan/UBSan selector 重复三次，均 exit code 0 且无 sanitizer 报告。带候选输出
目录优先的 `LD_LIBRARY_PATH` 运行 `DI_NativeRequester --help` 通过；此前 `/usr/local/lib` 优先的
loader 失败保留为库来源边界。详见 [B3 evidence](evidence/b3-checkpoint-export-20260911.md)。

2026-09-11 **B4-CALLER / CLOSED_FOR_VALIDATION**：T005 将五组维护入口收敛为 caller/mode
矩阵 12 行，明确 native owner、显式 compatibility、C++ selector、source/build closure、
zero-use 和 rollback。候选 provider 路径修正后，五个当前 C++ route/provider/Qwen selectors
均 exit 0，Python route/compatibility 回归 38 tests passed。B4 没有新增异步 owner，动态
profile 对 caller/launcher 记为 `none` with reason，B1/B2 的 TSan/ASan/UBSan 仍是共享状态机
的动态证据。旧 D2b selectors 在 bootstrap 后观测到零 response/role/output，作为 runtime/test
miss 保留；真实模型、no-Python、Python retirement 和外部实验仍未验收。详见
[B4 evidence](evidence/b4-caller-convergence-20260911.md) 与 [caller matrix](contracts/caller-matrix.md)。

2026-09-11 **B5-MATRIX / PARTIAL**：T006 已将 14 个继承父任务、16 个 `PO`、8 个 `I`、
19 个 `FR`、14 个 `CD` 和 9 个 `INV` 逐项加入 qualification matrix，共 80 行；矩阵字段
检查在修正继承旧行列数范围后通过，动态参数矩阵要求已同步到共享 skill、Spec Kit templates
和 Spec184。当前 `DEV-865e1ee2` 只是真实开发 checkpoint 关联标签，不是 promotion candidate；
native tokenizer/parser、完整 process/no-Python、真实模型、ordered candidate digest、
fresh convergence audit 和 T007 仍开放。详见 [B5 matrix evidence](evidence/b5-matrix-binding-20260911.md)。

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

- [x] T001 [US1] **Authority IO Dispatch**. FR-001；修复 F-01，在 `NativeAuthenticatedGrantClient::coreIssue` 将 `ServiceUser::RequestServiceTargeted` 封送到 Core `postToIo`，处理 dispatch 前取消、空 request ID、异常、timeout 与晚回调；在 `tests/integration-tests/di-native-requester-grant.t.cpp` 的 `Spec184AuthorityIoOwnership` 中验证线程 owner、真实 `ServiceUser` 状态和 bounded cleanup。Risk class: `concurrency/lifetime`; Dynamic profile: `tsan`; invariants: IO owner、pending-call balance、late callback no-op。Target: `integration-tests`（已由 `tests/wscript` 注册 TU）。Dependencies: documentation gate and migration baseline。Evidence: [B1 evidence](evidence/b1-request-correctness-20260911.md)。
- [x] T002 [US1] **Turn Publication Synchronization**. FR-002；修复 F-02，核对 `NativeInferenceClient` turn/attempt mutable-state、publication linearization 和 ticket 清理；在 `tests/unit-tests/di-native-client.t.cpp` 的 `Spec184TurnPublicationRace` 中覆盖 cancel/deadline/close/replacement。Risk class: `concurrency`; Dynamic profile: `tsan`; invariants: ticket publication/abort linearization、stale-token rejection、terminal callback fencing。Target: `unit-tests`（unit glob 注册）。与 T001 组成 B1，逐任务静态门后统一运行交错用例。Dependencies: T001 static gate。Evidence: [B1 evidence](evidence/b1-request-correctness-20260911.md)。
- [x] T003 [US1] **Durable Outcome Linearization**. FR-003；修复 F-03，client/coordinator publish 与 handle outcome 一致；在 `tests/integration-tests/ndnsf-di-core-flow.t.cpp` 的 `Spec184DurableOutcome` 中覆盖 publish 后阻塞 FINALIZE、取消和超时。Risk class: `lifetime/linearization`; Dynamic profile: `asan-ubsan`; invariants: journal/handle lifetime、publish-after-cancel fencing、residue=0；普通 C++ selector 通过，独立无抑制 sanitizer selector 三次通过。Target: `integration-tests`。Dependencies: B1 behavior exit。Evidence: [B2 evidence](evidence/b2-durable-outcome-20260911.md)。

## Phase 2: Export and Callers

- [x] T004 [US2] **Atomic Private Checkpoint Export**. FR-004；修复 F-04，`DI_NativeRequester` 以同目录临时文件、`0600`、write/`fsync`/close/rename/目录 `fsync` 安全导出，明确 symlink 不跟随、失败保留和 round-trip；补 `tests/unit-tests/di-native-checkpoint.t.cpp` 的 `Spec184CheckpointExport`。Risk class: `lifetime/serialization`; Dynamic profile: `asan-ubsan`; invariants: temp-file ownership、loader lifetime、old checkpoint preserved on pre-rename failure。普通 C++ selector、独立无抑制 ASan/UBSan selector（三次）及候选目录优先的 example loader smoke 通过；目录 `fsync` 真实错误仍为显式限制。Target: `unit-tests`。Dependencies: B2 exit。Evidence: [B3 evidence](evidence/b3-checkpoint-export-20260911.md)。
- [x] T005 [US2] **Maintained Caller Mode Closure**. FR-005；按 [caller matrix](contracts/caller-matrix.md) 盘点五组维护入口及其模式、默认路由、native/compatibility/removed 状态、C++ oracle、zero-use 和 rollback；按共享逻辑分组迁移并检查薄绑定。Risk class: `routing/lifetime`; Dynamic profile: caller/launcher rows use `none` with reason because no new async owner is introduced; shared owner profiles remain authoritative; bounded parameter matrix covers native, compatibility, invalid-config and shutdown boundaries。当前 C++ route/provider/Qwen selectors and 38 Python route tests passed；旧 D2b runtime miss、real model/no-Python and retirement remain T006/T007。Dependencies: B3 exit。Evidence: [B4 evidence](evidence/b4-caller-convergence-20260911.md)。

## Phase 3: Qualification and Delivery

- [ ] T006 [US3] **Inherited Obligation and Harness Closure**. FR-006；维护 [qualification matrix](contracts/qualification-matrix.md)，已逐项对账原14个 OPEN 父任务、PO-001–016及适用 I/FR/CD/INV（80 行），并分别给出 obligation、component、harness、identity 和 external-owner 行；仍需补残余组件/装配/tokenizer/host/绑定实现或 fixture、trace/marker/build identity 缺口。每个新增实现子组先静态门再定向 C++ 验证，候选冻结且控制性 finding 清零后运行 fresh convergence audit。Risk class: `qualification-evidence`; Dynamic profile: per inherited row, documentation reconciliation `none` with reason; invariants: source/artifact identity、selector-to-obligation mapping and bounded parameter matrix。Dependencies: B4 exit；矩阵对账已完成但验收仍 PARTIAL。Evidence: [B5 matrix evidence](evidence/b5-matrix-binding-20260911.md)。
- [ ] T007 [US3] **Current Native Qualification**. FR-006；仅在 T006 的 qualification matrix 完整且 `evidence/convergence-b5.md` 为当前 candidate 的 fresh `PASS` 后，按矩阵运行同源完整 unit/integration、YOLO/Qwen MiniNDN/no-Python 与检错负例；绑定源码/二进制/日志，区分局部 PASS 和正式 qualification；不重跑未受影响的历史实验。Risk class: `qualification-runtime`; Dynamic profile: per inherited row (at minimum `tsan` for concurrency rows and `asan-ubsan` for lifetime/parser rows); invariants: candidate identity、terminal cleanup、negative boundary。Dependencies: T006 static/focused exits and fresh convergence `PASS`。
- [ ] T008 [US3] **Native Development Handoff**. FR-006；同步 Design/API/使用说明、两个入口示例、最终源码基线、剩余外部实验 TRANSFERRED 状态；不得以文档移交替代本地资格。Risk class: `documentation`; Dynamic profile: `none` (no runtime state); invariants: evidence links and status agreement。Dependencies: T007 PASS。

## Dependencies & Execution Order

B1(T001/T002) → B2(T003) → B3(T004) → B4(T005) → B5(T006–T008)。
所有 T ID 属于184；引用182时必须加 Spec 前缀（例如 `182:T005`）避免歧义。
T005/T006 的子组在所属矩阵维护，不继续在 tasks.md 堆积上百个 G 编号。
静态审查通过但批次 C++ 验证或适用动态 profile 未通过时保持 `[ ]` / PARTIAL；每批只维护一个结果记录。
任务状态不能由 migration record、文档结构 PASS、CLI smoke 或 Python wrapper 结果提升；
T007 也不能替代 T001–T006 的定向 C++ 出口。

## Validation

每个任务的 C++ test/negative oracle 见 spec Acceptance Evidence Contract 和注册表；批次组合
审查通过后统一运行相关验证。T007 是最终 qualification，不替代前置批次定向 test，且必须
服从 [promotion candidate](contracts/promotion-candidate.md) 的失效矩阵。
