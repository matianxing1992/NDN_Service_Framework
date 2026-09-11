# Feature Specification: Native DI Closure

**Feature Branch**: `Experimental`
**Feature Directory**: `184-native-di-closure`
**Created**: 2026-09-11
**Status**: PLANNED / implementation NOT_STARTED
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
注册点见下方 [Native C++ Test Registration](#native-c-test-registration)；所有新 selector
当前仍为 `PLANNED`，未声称已经运行。

| Story / FR | Production entry / callers | Observable outcome | Independent oracle / C++ selector | Negative / recovery boundary | Evidence owner / path | Batch |
| --- | --- | --- | --- | --- | --- | --- |
| US1 / FR-001 | DI/NativeAuthenticatedGrantClient.cpp → ServiceUser::RequestServiceTargeted | Core IO owner 提交，有界晚回调 | integration-tests / `di-native-requester-grant.t.cpp` / `Spec184AuthorityIoOwnership` (PLANNED)；独立线程 ID 与事件记录 | dispatch 前取消、空 ID、异常、timeout | B1 实现者；evidence/ | B1 |
| US1 / FR-002 | DI/NativeInferenceClient.cpp ACK planning / markTerminal | turn/attempt 同步，唯一终态 | unit-tests / `di-native-client.t.cpp` / `Spec184TurnPublicationRace` (PLANNED)；barrier 与 pending ticket 计数 | cancel/deadline/close/replacement | B1 实现者；evidence/ | B1 |
| US1 / FR-003 | NativeInferenceClient → NativeConversationCoordinator | journal、checkpoint、handle 一致 | integration-tests / `ndnsf-di-core-flow.t.cpp` / `Spec184DurableOutcome` (PLANNED)；持久记录独立读取 | publish 前后取消、FINALIZE 丢失 | B2 实现者；evidence/ | B2 |
| US2 / FR-004 | examples/DI_NativeRequester.cpp checkpoint export | 首次0600、原子替换与可再加载 | unit-tests / `di-native-checkpoint.t.cpp` / `Spec184CheckpointExport` (PLANNED)；stat/原文件摘要/loader | 写入失败、symlink、已有文件 | B3 实现者；evidence/ | B3 |
| US2 / FR-005 | APPClient 与维护中的五组 caller/mode | 默认 runtime 原生、薄封装 | DI_NativeRequester / di-native-provider；integration-tests 中按当前 caller 清单登记 exact selector | 旧模式退出、unsupported 模式明确拒绝 | B4 实现者；contracts/caller-matrix.md（T005 生成） | B4 |
| US3 / FR-006 | 同源 unit-tests / integration-tests / C++ process / MiniNDN | 继承 PO/I 及 no-Python 出口可追溯 | 原 Spec182 proof-design 中 PO-001–016；T006 逐项绑定当前 exact selector | 所有继承负例、build identity、trace/marker 首边界 | B5 实现者；contracts/qualification-matrix.md（T006 生成） | B5 |

### Edge Cases

IO 队列积压、取消先于/后于 publish、晚回调、replacement attempt2、Provider stateMissing、
丢失 FINALIZE、宽权限旧文件、错误构建树、collector 不完整、模型/实验机不可用均须显式分类。
stateMissing 拒绝不是 durable KV recovery；本 Spec 不新增透明跨 Provider KV 迁移目标。
Checkpoint destination 若为 symlink，导出不得跟随或改写其 target；实现必须在同一目录创建
受限临时文件（首次可见为 `0600`），完成 write、`fsync`、close 后以原子 rename 替换目录项，
并在目录 `fsync` 后返回。临时文件或持久化失败时保留旧目录项；symlink target 本身不变。

### Native C++ Test Registration

| Selector | Test source | Target | Registration contract | Status |
| --- | --- | --- | --- | --- |
| `Spec184AuthorityIoOwnership` | `tests/integration-tests/di-native-requester-grant.t.cpp` | `integration-tests` | already in `tests/wscript` integration source list; add selector and verify exact command | PLANNED |
| `Spec184TurnPublicationRace` | `tests/unit-tests/di-native-client.t.cpp` | `unit-tests` | covered by `tests/wscript` unit `ant_glob`; add selector and verify exact command | PLANNED |
| `Spec184DurableOutcome` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp` | `integration-tests` | already in `tests/wscript` integration source list; add selector and verify exact command | PLANNED |
| `Spec184CheckpointExport` | `tests/unit-tests/di-native-checkpoint.t.cpp` | `unit-tests` | new TU must be added under the unit glob and verified in the target closure | PLANNED |

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
