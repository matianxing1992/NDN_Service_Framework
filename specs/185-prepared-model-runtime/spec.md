# Feature Specification: Prepared Model Runtime

**Feature**: 185-prepared-model-runtime | **Branch**: Experimental | **Date**: 2026-09-12
**Status**: COMPLETE — B0–B9 实现、原生验收、Python 绑定验收和设计/API 交付已完成；Spec184 外部资格边界仍按其自身状态维护。
**Input**: 用户提供的 Runtime/User/PreparedModel 草案，审计后以本目录契约为准。

## Summary

应用先准备一个绑定模型、任务、adapter 和拆分目录的可复用对象，然后只提交输入及必要的请求选项。
准备成功意味着本机已经验证并可使用不可变模型包；不意味着获得执行权限、找到 Provider、
预留资源或创建远端 runner。NativeInferenceClient 继续作为唯一请求执行器，逐步退到公开 façade 后面。
独立 artifact authority、Core Request/ACK/Selection/Response 和既有会话事务保持其所有权。

完整能力首先由独立C++ SDK和生产进程提供，Python核心对象由pybind11直接绑定，便利层只转换参数、异常、GIL与asyncio；详见[C-06](contracts/cpp-first.md)。
[C-07全API及生命周期](contracts/api-catalog.md)列出全部新稳定application/provider入口、Python映射、值类型、析构与关闭规则。B0–B9 已按该契约实现并验收；subinterpreter、wheel、Spec184外部模型和SIF/Tiger等明确未观测边界不在本 Spec 的完成声明中。
全表面盘点见[API review](api-review.md)，不等于8500项声明的完整正确性证明。

## User Scenarios & Testing

### User Story 1 - Prepare Once and Reuse (Priority: P1)

应用准备一次模型，连续或并发提交多个输入，不再传 model/source/splitter/catalog/grant。
**Independent Test**: C++ 公开入口两次 prepare、两次 request；计数 oracle 证明相同准备键只解析一次，
每个请求仍有不同的 native request/attempt 和独立授权检查。
**Acceptance Scenarios**: 冷缓存完成准备；热缓存返回独立 receipt；同键并发共享准备；
错误 digest、非 ONNX source、配置冲突、超时和取消不发布 READY；缓存预算不足明确失败。

### User Story 2 - Keep Request Security and Lifecycle (Priority: P1)

应用通过简洁句柄观察结果、流和会话；缓存不能绕过撤销、ACK admission 或终态一致性。
**Independent Test**: C++ 请求真实经过 ACK_CLOSED、placement、grant、Selection 和 Response；
两次请求之间撤销权限，第二次即使缓存命中仍拒绝。
**Acceptance Scenarios**: 默认与覆盖 placement、拒绝错误 role/provider、局部 wait 超时不取消请求、
cancel/close/完成竞争、两轮会话持久提交及恢复、错误 manifest checkpoint 拒绝。

### User Story 3 - Prepare Provider Runners After Selection (Priority: P2)

服务提供者注册能力，只有认证 Selection 才能触发需要授权的模型获取与 runner 准备。
**Independent Test**: C++ Provider 无有效 Selection 时 fetch/assembly 计数为零；有效 Selection
触发准备，相同执行身份可复用不可变 artifact，不同请求的可变 KV/runner 状态隔离。
**Acceptance Scenarios**: 同身份命中、ABI/精度/角色变化不误命中、撤销拒绝、组装失败清理、stop drain。

### User Story 4 - Standalone C++ SDK and Thin Python (Priority: P2)

C++ example 与 Python 薄封装使用同一个 PreparedModel 后端。维护调用方有逐项迁移或保留原因。
**Independent Test**: 同一模型/输入的新旧入口结果及失败语义对照，C++ 生产进程先通过，之后验证 Python façade。

## Functional Requirements

- **FR-001** **Runtime Ownership**: Runtime 明确拥有 IO/executor/用户凭据/准备缓存；不持有 authority 签名私钥；关闭有 drain 出口。
- **FR-002** **Verified Package Identity**: prepare 从显式可信配置解析精确模型/任务/source；配置摘要、内容摘要与来源信任分别验证。
- **FR-003** **Bounded Preparation Cache**: 四种缓存策略具有互斥且可测试的语义；single-flight、独立等待期限、lease、预算和 Refresh 失败原子性遵循 C-02。
- **FR-004** **Prepared Request Submission**: PreparedModel 绑定不可变模型和 splitter；输入编码、动态候选预算、ACK admission、placement、grant 和 plan 仍属于本次请求。
- **FR-005** **Handle and Event Contract**: 按C-07完整定义同步/异步局部等待、Subscription、退订不吞事件、流错误/EOF、析构和关闭后访问；不伪造远端状态。
- **FR-006** **Conversation Ownership**: 会话绑定模型/任务身份并串行提交 turn；复用现有 journal、receipt/control、recovery/replacement 机制。
- **FR-007** **Post-Selection Assembly**: Provider façade 委托现有认证后准备链，runner cache 不充当权限凭据或共享可变会话状态。
- **FR-008** **Compatibility and Export**: 公共 C++ 头、安装目标和 Python façade 可消费；迁移清单覆盖实际维护调用方，旧签名过渡期保留，禁止静默 fallback。
- **FR-009** **Evidence and Documentation**: 每任务静态门、批末共享构建/测试与动态卡；产品验收使用 C++ fixture/driver/oracle；Design 当前与目标独立同步。

- **FR-010** **Installed API Boundary**: C-05六层暴露清单、安装头闭包和ONNX稳定ABI由安装prefix外部consumer验证。
- **FR-011** **Extension Lifecycle**: 注册freeze、duplicate/replace、runner隔离及协作deadline/cancel遵循C-05，不承诺任意插件抢占。
- **FR-012** **Independent C++ Capability**: C-06/C-07全部C++入口与生命周期原生实现；Python核心对象直接绑定、便利层不持领域状态；逐行exposure/消费/绑定验收，先T013再T012。

- **FR-013** **Reusable Core Ownership**: [C-09](contracts/core-app-boundary.md)通用调度、ticket、完成等待、订阅/reader由Core实现；DI仅保留领域状态与包装，复用已有四消息/协作/流/注册入口；Core不得依赖DI/Python/ONNX。T017/T018先于T001。

## Success Criteria

- **SC-008** **Core Reuse Proof**: C-09 PO-C1–C4全部通过；Core-only非模型消费者独立链接，真实DI请求委托同一通用实现且保留领域提交顺序，受影响Core回归通过。

- **SC-001** **Reuse**: 同一 key 的 8 个并发 prepare 只有一次 source 获取/验证/检查；随后 2 次请求不重复完整 source 解析，且有各自 request identity。
- **SC-002** **Isolation**: C-02 缓存策略矩阵全部符合预期；任一等待者取消不会终止其他有效等待者；失败不产生可见半成品。
- **SC-003** **Security**: 热缓存撤销、错误 Selection、跨主体缓存、错 digest/任务、过期 grant 的反例均拒绝且无未授权 runner 执行。
- **SC-004** **Lifecycle**: unary、stream、两轮 continuation、recovery、replacement、cancel/close 的具名 C++ 用例满足独立 oracle，终态单一且 drain 后无活动 owner。
- **SC-005** **Migration**: 每个维护调用方有可验证分类；至少 C++ requester/provider 和一个 Python facade 走新入口，兼容语义差异显式列出；不以 Python 结果替代 native 资格。

- **SC-006** **Standalone Consumption**: 外部consumer仅用安装SDK覆盖C-06全部模式，安装头独立编译、ABI及运行闭包通过；不依赖Python解释器、libpython或helper。
- **SC-007** **Usability and Extension Contracts**: C-05/C-06错误/时间/能力查询、可靠completion/EventReader、冻结注册和协作控制反例由C++用例通过；Python另验映射语义，不代表用户研究结论。

## Scope and Dependencies

[C-08 Implementation Design](contracts/code-design.md)明确全部实现任务的类/文件delta、关键前后签名、字段owner、调用与失败流程、PO及Design binding。
现有C-07公开清单不替代内部设计；受影响任务开工前核对当前源码及就绪范围。T016合作策略/catalog先于T003 Package构造，见plan实际顺序。

Spec184 T007/T008、Qwen3.6-27B 外部资格和旧路径 retirement 未完成项继续归184，本 Spec 不自动转移或关闭。
185 不以184整个外部实验完成作为纯本地缓存工作的前提；B3 接入前需要对所依赖的184请求/安全/终态基线
完成定向回归并登记 source identity。相同文件不得由两条实现线并发修改；阶段开始先确认当前 HEAD/diff。
新 façade 导致的回归归185；原有未修改失败留184并链接。185的完成仅表示本规格范围通过，不表示184整体资格通过。

本期缓存限定一个 Runtime 内的内存准备目录与现有 source 存储 lease；不新建跨进程共享缓存数据库、
远程模型搜索协议或公开任意 URL 下载器。不增加新的 Core wire 状态。Provider prewarm 不列入此版公开 API。

## Acceptance Evidence Contract

下表保留设计阶段注册的 suite 名称；当前运行状态与真实结果以 `tasks.md` 及各批次 evidence 为准。名称对应已注册的 Boost C++ selector，未观测边界不会因文档登记而升级为 PASS。
生产库为现有 `ndnsf-distributed-inference`（Waf 注册在根 `wscript:587`），selector 归现有
`unit-tests` 或 `integration-tests`；T001 必须核对实际 target/output 并登记映射，不能猜 build 路径。

| Requirement / Story | Contract | Task owner | Production entry / C++ oracle | Evidence |
| --- | --- | --- | --- | --- |
| FR-013 / US1,US2 | C-09 | T017,T018 | Core OperationRuntime/State and NativeInferenceClient; Spec185CoreOperation / Spec185DiCoreOperation | evidence/b0c-core-operation.md |
| FR-001 / US1 | C-01 | T001,T002 | Runtime::open/close/drain; Spec185Runtime ownership counters | evidence/b1-runtime.md |
| FR-002,FR-003 / US1 | C-02 | T003,T004 | User::prepare; Spec185Preparation source/parser counters and byte oracle | evidence/b2-preparation.md |
| FR-004,FR-005 / US2 | C-01,C-03 | T005,T006 | PreparedModel::request; Spec185PreparedRequest signed native trace and independent result | evidence/b3-request.md |
| FR-006 / US2 | C-03 | T007,T008 | Conversation::request/exportCheckpoint; Spec185Conversation journal/handle equality | evidence/b4-conversation.md |
| FR-007 / US3 | C-03 | T009,T010 | Provider::serve; Spec185ProviderAssembly fetch counter and state isolation | evidence/b5-provider.md |
| FR-008 / US4 | C-01,C-04 | T011,T012 | DI_NativeRequester; Spec185Compatibility and wrapper-only checks | evidence/b6-migration.md |
| FR-009 / all | C-04 | T013,T014 | same-tree native process + source/ELF identity + final design comparison | evidence/b7-cpp-qualification.md |

| FR-010 / US4 | C-05,C-06 | T015 | installed consumer; Spec185InstalledApi | evidence/b0-installed-api.md |
| FR-011 / US4 | C-05 | T016 | registry/strategy/runner; Spec185ExtensionRegistry | evidence/b2e-extensions.md |
| FR-012 / US1–US4 | C-05,C-06 | T003,T006,T009,T011,T013,T012 | complete C++ process/async before binding | evidence/b7-cpp-qualification.md |

## Design Index

[Audit and decisions](audit.md) · [Research](research.md) · [Plan](plan.md) · [Tasks](tasks.md) ·
[Data model](data-model.md) · [C-01 Public API](contracts/public-api.md) ·
[C-02 Preparation](contracts/preparation.md) · [C-03 Execution](contracts/execution.md) ·
[C-04 Validation](contracts/validation.md) · [C-05 Usability](contracts/api-usability.md) · [C-06 C++ First](contracts/cpp-first.md) · [Traceability](traceability.md) · [Quickstart](quickstart.md)
