# Feature Specification: Native DI Closure

**Feature Branch**: `Experimental`
**Feature Directory**: `184-native-di-closure`
**Created**: 2026-09-11
**Status**: IN_PROGRESS / B5 component and full-unit exits recorded; integration qualification pending
**Input**: 用户要求将过度膨胀的 Spec182 未完成工作迁入 Spec184；以最新请求链审计安排剩余执行。

## Scope

承接 Spec182 的四项源码缺陷、维护调用方收敛以及尚未完成的原生资格与交付。
保留已经实现的 C++ 库、独立 authority、requester/Provider、stream 和 continuation；
不重新设计 NDNSF 四消息流程，不重做已有组件，不把历史 PASS 变为本 Spec 的新 PASS。
来源和全部14个未完成父任务见 [transfer matrix](contracts/transfer-matrix.md)。
Spec182 作为历史基线保留，状态为 TRANSFERRED / qualification INCOMPLETE，不能标成已完成。

## User Scenarios & Testing

### User Story 1 - Consistent Request Outcome (Priority: P1)

调用者在 grant、规划或会话提交期间取消/超时，仍能得到与真实持久状态一致的唯一结果。
**Why this priority**: 当前有线程竞争和已提交却报告失败的窗口。
**Independent Test**: 用生产 C++ client/transport 与确定性交错 fixture 检查提交线程、终态和 journal。
**Acceptance Scenarios**:
1. **Given** authority 请求尚在 IO 队列，**When** 取消先发生，**Then** 不再启动失效请求。
2. **Given** turn 正在绑定 role map，**When** cancel/deadline/close 并发，**Then** 无数据竞争或遗留 ticket。
3. **Given** parent 已持久提交，**When** FINALIZE 延迟且取消到达，**Then** handle 不降为普通失败。

### User Story 2 - Safe Continuation and Native Callers (Priority: P2)

用户能安全保存续聊 checkpoint，并通过维护中的默认入口使用同一原生实现。
**Independent Test**: 文件权限/失败恢复 C++ fixture，以及逐 caller/mode 的生产 C++ oracle 和薄封装检查。
**Acceptance Scenarios**:
1. **Given** umask 022，**When** 导出 transcript，**Then** 从首次可见起为0600，失败保留旧 checkpoint。
2. **Given** 某维护调用方的支持模式，**When** 使用默认入口，**Then** 原生 owner 完成运行逻辑，兼容入口显式标识。

### User Story 3 - Traceable Native Qualification (Priority: P2)

维护者能区分实现、定向通过、正式资格与外部实验，且旧验收义务不因拆 Spec 消失。
**Independent Test**: 逐条验收矩阵核对源码/二进制身份、生产 C++ target 与原始结果。
**Acceptance Scenarios**:
1. **Given** 旧 selector PASS，**When** 接口或共享源码改变，**Then** 只按影响决定复验，不能无条件沿用。
2. **Given** no-Python 或某模型出口未观测，**When** 汇总交付，**Then** 保持 PARTIAL/NOT_RUN，不报全链完成。

## Acceptance Evidence Contract

`DI/` 指 `NDNSF-DistributedInference/cpp/ndnsf-di/`。selector、source TU、target 和
注册点见下方 [Native C++ Test Registration](#native-c-test-registration)。表中的
`FOCUSED_VALIDATED` 只表示当前候选的定向 C++ 出口已运行；它不表示跨进程、no-Python 或
formal qualification 完成。

动态验证按共享 [batch-quality-gates](../../skills/speckit-code-design/references/batch-quality-gates.md)
的风险 profile 执行，不按每个小任务重复构建。并发/IO owner/取消与替换使用 `tsan`；
生命周期、损坏 checkpoint 和未定义行为使用 `asan-ubsan`；不可信 wire/JSON 解析才使用
`parser-fuzz`。每个 profile 必须绑定 C++ selector、独立输出目录、重复次数或预算以及
可观察不变量；`NOT_RUN`、`DYNAMIC_PASS` 和 `DYNAMIC_FAIL` 单独记录，动态通过不能替代
行为或 qualification 通过。

每个逻辑批次在组合审查后维护一张 `Dynamic gate card`，统一登记参数/边界、生产 C++
selector、业务不变量、重复或 fuzz 预算、toolchain/source identity、输出路径和失败分类。
动态工具不能判断参数是否满足模型或协议语义；该判断必须由直接调用生产 C++ target 的
fixture/oracle 完成。若 sanitizer 报告来自外部库或 ABI 边界，先保留未抑制日志并保持
`DYNAMIC_FAIL`/`PARTIAL`；只有一致 ABI 构建下无抑制重跑干净，才可写 `DYNAMIC_PASS`。
这张卡属于批次证据，不为每个小任务复制构建和报告。

每张卡还必须维护有界 `Dynamic Parameter Matrix`，按行为等价类覆盖正常值、关键最小/最大
值、故意非法值，以及会改变取消、deadline 或 replacement 顺序的用例。每一行写出参数来源、
确定性 seed（若适用）、预期业务结果和对应的 C++ 断言；未覆盖的边界转交 qualification
matrix，并说明理由。矩阵只属于批次，不把每个参数拆成任务。

### Dynamic Validation Procedure

Spec184 的动态门固定为四步，且只在批次级执行：

1. **Freeze**：批次达到稳定行为出口后，完成静态五 lane 覆盖和组合审查，冻结
   `Dynamic gate card` 的 risk/profile、C++ selector、源码/工具链身份、输出路径和预算。
2. **Sample**：按风险和行为等价类选取少量 `nominal`、关键边界、故意非法及
   cancel/deadline/replacement 顺序用例；不按 80 行 qualification matrix 建 80 套动态任务。
3. **Run**：运行独立 ASan/UBSan、TSan 或 parser-fuzz 构建中的具名 C++ selector；业务结果
   由 C++ fixture/oracle 断言，Python 只能编排外部设施或启动该 executable。
4. **Classify**：记录每个 case 的 `DYNAMIC_PASS`、`DYNAMIC_FAIL` 或 `NOT_RUN`、首个失败
   边界、退出码和清理结果。未覆盖项或失败项保留在 qualification row，不提升任务状态。

B5 以继承 obligation 的**不同风险/行为类别**作为动态样本单位，每类默认一条正例和一条
负例，具体预算由卡片冻结；相同状态机、selector 和 source closure 的行共享一次构建。80 行
矩阵仍用于逐项资格对账，不能被动态样本数量替代。动态结果只能补充 C++ 行为测试，不能把
无报告或单次启动成功写成 `QUALIFICATION_PASS`。

| Batch | Parameter classes / budget | Expected C++ business result | Dynamic selector / invariant | Uncovered boundary / handoff |
| --- | --- | --- | --- | --- |
| B1 | request/attempt nominal + cancel/deadline/replacement; bounded repeats | one terminal result, no stale callback, pending count returns to zero | `Spec184AuthorityIoOwnership`, `Spec184TurnPublicationRace` / IO owner and ticket balance | external process interleavings → B5 qualification |
| B2 | publish before/after cancel, delayed FINALIZE, close; bounded repeats | handle/journal linearization remains monotonic and residue is zero | `Spec184DurableOutcome` / durable outcome and cleanup | cross-process durability → B5 |
| B3 | umask, existing file, symlink, write/fsync/rename failure; bounded cases | exact `0600`, canonical bytes, old file preserved on pre-rename failure | `Spec184CheckpointExport` / export atomicity and residue | directory fsync failure after rename is explicit implementation limit |
| B4 | native/compatibility mode, missing/invalid config, caller shutdown; per-row bounded cases | native route is explicit, compatibility is explicit, no hidden fallback | caller-specific C++ selectors / route and lifecycle markers | real model/no-Python outputs → B5 |
| B5 | one positive and one negative sample per distinct inherited risk/behavior class; matrix-owned budget | candidate identity, terminal cleanup, and negative boundary each have evidence | matrix-bound C++ selectors / source-artifact identity | unrepresented rows remain qualification `PARTIAL`; external Tiger/SIF runs remain `TRANSFERRED` |

| Story / FR | Production entry / callers | Observable outcome | Independent oracle / C++ selector | Negative / recovery boundary | Dynamic profile / invariant | Evidence owner / path | Batch |
| --- | --- | --- | --- | --- | --- | --- | --- |
| US1 / FR-001 | DI/NativeAuthenticatedGrantClient.cpp → ServiceUser::RequestServiceTargeted | Core IO owner 提交，有界晚回调 | integration-tests / `di-native-requester-grant.t.cpp` / `Spec184AuthorityIoOwnership` (`FOCUSED_VALIDATED`)；独立线程 ID 与事件记录 | dispatch 前取消、空 ID、异常、timeout | `tsan`; IO owner、pending-call 平衡、迟到回调无副作用 | B1 实现者；evidence/b1-request-correctness-20260911.md | B1 |
| US1 / FR-002 | DI/NativeInferenceClient.cpp ACK planning / markTerminal | turn/attempt 同步，唯一终态 | unit-tests / `di-native-client.t.cpp` / `Spec184TurnPublicationRace` (`FOCUSED_VALIDATED`)；barrier 与 pending ticket 计数 | cancel/deadline/close/replacement | `tsan`; ticket 发布与 abort 线性化、终态后拒绝旧 token | B1 实现者；evidence/b1-request-correctness-20260911.md | B1 |
| US1 / FR-003 | NativeInferenceClient → NativeConversationCoordinator | journal、checkpoint、handle 一致 | integration-tests / `ndnsf-di-core-flow.t.cpp` / `Spec184DurableOutcome` (`FOCUSED_VALIDATED`)；持久记录独立读取 | publish 前后取消、FINALIZE 丢失 | `asan-ubsan`; handle/journal 生命周期和 residue=0 | B2 实现者；evidence/b2-durable-outcome-20260911.md | B2 |
| US2 / FR-004 | examples/DI_NativeRequester.cpp checkpoint export | 首次0600、原子替换与可再加载 | unit-tests / `di-native-checkpoint.t.cpp` / `Spec184CheckpointExport` (`FOCUSED_VALIDATED`)；stat/原文件摘要/loader | 写入失败、symlink、已有文件 | `asan-ubsan`; 临时文件/loader 生命周期、失败保留旧文件 | B3 实现者；evidence/b3-checkpoint-export-20260911.md | B3 |
| US2 / FR-005 | APPClient 与维护中的五组 caller/mode | 默认 runtime 原生、薄封装 | DI_NativeRequester / di-native-provider；integration-tests 中按当前 caller 清单登记 exact selector | 旧模式退出、unsupported 模式明确拒绝 | `tsan` only for async callers; otherwise `none` for static matrix | B4 实现者；contracts/caller-matrix.md（T005 生成） | B4 |
| US3 / FR-006 | 同源 unit-tests / integration-tests / C++ process / MiniNDN | 继承 PO/I 及 no-Python 出口可追溯 | 原 Spec182 proof-design 中 PO-001–016；T006 逐项绑定当前 exact selector | 所有继承负例、build identity、trace/marker 首边界 | per inherited row as matrix metadata; dynamic execution samples distinct risk/behavior classes; `none` for documentation-only reconciliation | B5 实现者；contracts/qualification-matrix.md（T006 生成） | B5 |

### Edge Cases

IO 队列积压、取消先于/后于 publish、晚回调、replacement attempt2、Provider stateMissing、
丢失 FINALIZE、宽权限旧文件、错误构建树、collector 不完整、模型/实验机不可用均须显式分类。
stateMissing 拒绝不是 durable KV recovery；本 Spec 不新增透明跨 Provider KV 迁移目标。
Checkpoint destination 若为 symlink，导出不得跟随或改写其 target；实现必须在同一目录创建
受限临时文件（首次可见为 `0600`），完成 write、`fsync`、close 后以原子 rename 替换目录项，
并在目录 `fsync` 后返回。临时文件或持久化失败时保留旧目录项；symlink target 本身不变。

### Native C++ Test Registration

| Selector | Test source | Target | Registration contract | Dynamic profile | Status |
| --- | --- | --- | --- | --- | --- |
| `Spec184AuthorityIoOwnership` | `tests/integration-tests/di-native-requester-grant.t.cpp` | `integration-tests` | registered in `tests/wscript`; exact selector and TSan evidence in `evidence/b1-request-correctness-20260911.md` | `tsan`; repeat bounded selector | FOCUSED_VALIDATED |
| `Spec184TurnPublicationRace` | `tests/unit-tests/di-native-client.t.cpp` | `unit-tests` | covered by `tests/wscript` unit `ant_glob`; exact selector and TSan evidence in `evidence/b1-request-correctness-20260911.md` | `tsan`; repeat interleaving | FOCUSED_VALIDATED |
| `Spec184DurableOutcome` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp` | `integration-tests` | registered in `tests/wscript`; normal and rebuilt unsuppressed sanitizer evidence in `evidence/b2-durable-outcome-20260911.md` | `asan-ubsan`; cancel/finalize negative cases | FOCUSED_VALIDATED |
| `Spec184CheckpointExport` | `tests/unit-tests/di-native-checkpoint.t.cpp` | `unit-tests` | registered under the unit glob; normal and rebuilt unsuppressed sanitizer evidence in `evidence/b3-checkpoint-export-20260911.md` | `asan-ubsan`; malformed/symlink cases | FOCUSED_VALIDATED |

These registrations are design-time obligations, not evidence. A missing source, target, selector,
or registration is a `gap`; it cannot be reported as `STATIC_PASS` or product `PASS`. Native
runtime behavior and its primary assertions remain C++; Python may only cover facade shape,
configuration rejection, or orchestration.

## Requirements

### Functional Requirements

- **FR-001** **Authority Thread Ownership**: authority 请求的 Core 状态变更必须在 IO owner 上执行，等待与异常传播有界。
- **FR-002** **Synchronized Request State**: turn/attempt 发布与取消、超时、close、替换保持同步和单一终态。
- **FR-003** **Durable Outcome Consistency**: 持久提交与成功结果归属具有单一线性化决定；发布前取消不得提交，发布后不得降为普通失败。
- **FR-004** **Secure Checkpoint Export**: transcript 导出从首次可见起受限权限，失败不破坏已有有效 checkpoint，并能重新加载。
- **FR-005** **Native Default Callers**: 维护中支持的 caller/mode 默认进入同一 C++ owner；兼容入口不拥有已要求迁移的运行时逻辑。
- **FR-006** **Inherited Qualification and Handoff**: 不丢失原未完成任务、PO/I、no-Python、依赖闭包、局部/全量验收与交付边界；所有最终结论绑定实际源码和证据。

### Key Entities

Operation、NativeConversationTurn、durable checkpoint、caller/mode、qualification row 与 transfer row
沿用既有定义；本次拆分不改变 wire schema 或公开 API。

## Success Criteria

### Measurable Outcomes

- **SC-001** 所有声明的取消/超时交错用例只有一个与实际提交状态一致的结果，无未释放 ticket。
- **SC-002** 所有导出失败用例保留旧有效文件；成功文件符合权限和重新加载要求。
- **SC-003** 当前维护 caller/mode 清单100%有明确路由和验收归属，无隐式旧运行逻辑。
- **SC-004** 原14个未完成父任务、PO-001–016及适用 I 矩阵100%有承接状态和证据责任人；不能用“已转移”计成功。
- **SC-005** 最终本地资格逐项通过并可由源码/二进制身份复核；外部实验单列，不冒充完成。

## Assumptions

继续在 Experimental 开发，无新长期分支、不自动 push。C++ code → 逐任务静态门 → 批次组合审查
→ 共享 C++ build/test/process → Python wrapper；SIF/Tiger 由实验机负责。
迁移只改变文档和调度，不改变已接受的产品目标。后续 API/行为修复仍同步 Design 当前/目标契约。
