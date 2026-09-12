# C-09 Core and Application Ownership

**Status**: PLANNED | **Baseline**: b0b8ada1 / 2026-09-12

## Source Audit

当前只部分满足边界。`ndn-service-framework/ServiceUser.hpp:851–917`及
`ServiceUser.cpp:6756–7005`已有BeginCollaboration、ACK关闭、CommitCollaborationPlan和CancelCollaboration；
提交检查ACK摘要、期限和计划完整性。`InvocationStream.hpp:212–279,489–658`已有流handle、取消及唯一传输终态。
`ServiceProvider.hpp:844–873,1279–1323`已有scoped registration及RAII关闭。
DI的`NativeInferenceClient.cpp:174–289`仍自建SerialRequestExecutor，`:291–350`把领域状态与mutex/CV、deadline、通知队列混合；
`:1214,1350`实际调用Core提交/发起协作，没有另一套四消息传输。
因此问题是通用运行时复用不足，不能宣称调用协议全在DI或需重写协议。

## Ownership Decision

| Responsibility | Current | Target / task |
| --- | --- | --- |
| Request / ACK / Selection / Response、权限验证、协作提交及取消 | Core | REUSE既有ServiceUser入口；T018,T005不得另造协议 |
| 流加密、排序、重传、传输终态 | Core InvocationStream | REUSE；SDK消费队列不替代网络队列或传输终态 |
| worker/通知调度、工作ticket、close/drain屏障 | DI自建；185原CD01拟继续在DI扩充 | Core运行时primitive；T017，DI组合使用 |
| completion等待、退订、可靠事件读取及容量限制 | DI现有观察队列；185原CD04计划新增 | Core通用primitive；T017,T018，DI仅类型转换和领域完成判定 |
| 服务注册generation/fence/RAII | Core已有；DI提供host | REUSE scoped registration；DI Provider不复制注册状态机 |
| Package、模型catalog、split/placement评分、tensor/backend/runner | DI | KEEP；模型知识不能进入Core |
| token前缀、KV receipt、会话journal、COMMIT/ROLLBACK/FINALIZE语义 | DI并借Core安全传输 | KEEP；本期不抽象通用分布式事务，不把stream final等同conversation commit |
| 模型artifact缓存key、受保护组装与租约 | DI并借Core数据传输/授权 | KEEP；以后其他应用有同构需求再抽象缓存策略 |

## Concrete Design Binding

以下是C-08的owner修订，公开C-07签名及容量/生命周期语义继续有效。
Core目录为`ndn-service-framework/`，命名空间为`ndn_service_framework`；新符号均未实现。

**CB01 / T017**：ADD `OperationRuntime.hpp/.cpp`，定义`OperationRuntime`、move-only `WorkTicket`、
move-only `OperationSubscription`；抽出DI SerialRequestExecutor的调度能力，保留测试注入在内部测试端口。
`OperationRuntime::create()`返回shared_ptr；`acquire()`返回WorkTicket；
`post(WorkTicket&, std::function<void()>)`排队或抛关闭错误；
`close() noexcept`拒绝新ticket，已登记清理仍可运行；`drain(milliseconds)->bool`；
`drainAsync(milliseconds, std::function<void(bool)>)->OperationSubscription`。
Core保留该签名作为显式shutdown barrier；另提供同签名加`closeRuntime=false`的
内部组合入口，用于DI `Runtime::drainAsync`在不改变Open状态时等待当前工作静止。
该组合入口不把quiescent通知写成Core `drained`终态；只有Core已关闭且所有ticket、
queued、active及timer清空时才发布`drained`。
ticket保活运行时，析构恰好减一次计数；排队任务额外持ticket的内部共享状态，不能依赖引用参数寿命。
State持phase、mutex、ticket计数、worker/notification队列和安全join owner；不能依赖DI、Python、ONNX。
post不能在调用者锁内执行callback；drain禁止在自己worker/Face owner线程阻塞；回调内close不自join。
drain排除自身最终通知；already-drained回调遵守C-07的锁外inline契约。
Runtime只拥有自己创建的线程；借用Face/io_context通过显式keep-alive owner保活至清理完成，不擅自停止外部event loop。

**CB02 / T017**：ADD `OperationState.hpp`，模板`OperationState<Result, Event>`与`OperationReader<Event>`；
`OperationSubscription::cancel() noexcept`只取消等待/通知，不能取消业务。
关键方法：`complete(Result)->bool`、`fail(std::exception_ptr)->bool`、
`result(milliseconds)->Result`、`onCompletion(std::function<void()>)->OperationSubscription`、
`resultAsync(milliseconds,std::function<void(std::optional<Result>,std::exception_ptr)>)->OperationSubscription`、
`publish(Event,size_t encodedBytes)->bool`、`openReader()->OperationReader<Event>`。
Reader提供`next(milliseconds)->optional<Event>`、
`nextAsync(milliseconds,std::function<void(optional<Event>,exception_ptr)>)->OperationSubscription`及`close() noexcept`。
初始化时必须注入shared OperationRuntime、业务取消函数、事件字节计量及错误映射；
Core提供`OperationErrorCode{Closed,Timeout,Cancelled,Capacity,EventGap,WouldDeadlock}`，DI显式映射到DiError。
State的mutex下唯一裁决Pending→Success/Failure，结果只读；只有领域owner能调用complete/fail，reader只读。
State不解释Result/Event。64 completion/wait slots、64 observer slots、1024 events/16MiB和单reader规则沿C-07；
`observe(std::function<void(const Event&)>)->OperationSubscription`为独立best-effort通道。
容量超限不能伪装EOF；失败前已收事件按C-07排空后抛错，读取消不提前推进cursor。
本地等待超时不改变业务终态；close拒绝新订阅但最终同步result可读。不得直接复用传输终态claim作为业务成功。

**CB03 / T018**：MODIFY DI `NativeInferenceClient.hpp/.cpp`，移除私有SerialRequestExecutor实现；
Operation组合Core运行时和`OperationState<NativeInferenceResult,NativeInferenceEvent>`，保留planned、conversation、
token、attempt和授权字段。旧request/result/cancel/observe签名兼容；NativeRequestAccess仍为内部桥接，但只委托Core等待/订阅/reader。
通用状态只存一份；原mutex可保护领域投影，但不再独立实现第二套result/CV/通知终态。
现有NativeRequestStatus通过Core完成态与DI阶段投影，不能维护可分歧的双终态。
DI取消函数仍调用既有取消/rollback路径；不得由通用State提前宣布durable成功。

**CB04 / T001,T002,T004,T006,T009**：DI RuntimeState持Core OperationRuntime和model/client registry，
F01/F02的通用phase/ticket/worker迁到CB01；F09/F10/F11/F12中的等待/订阅/消费队列迁到CB02。
DI RequestHandle/EventReader/Subscription保留C-07公开名称和类型适配，不实现通用调度。
模型prepare的single-flight/generation仍由DI管理；每个waiter组合Core State，最后外部handle释放只取消该waiter。
Provider包装既有Core ServiceRegistration；模型准备和领域cleanup完成后归还Core ticket。
安装Core新头与库符号，DI依赖方向仅DI→Core；不能用Core包含DI头或通过回调暗中要求加载DI库。

## Call and Failure Flow

DI PreparedModel → DI输入/模型投影 → Core工作ticket → 原生DI准备/规划回调 →
Core BeginCollaboration → ACK_CLOSED → DI split/placement → Core CommitCollaborationPlan →
Core Provider执行派发/Response或stream → DI结果与会话校验/提交 → Core OperationState完成 → DI类型化handle。
Core负责传输合法性，DI负责领域完成性；两种状态有明确层次，不能合并成功时机。
任何同步提交异常必须释放ticket并完成相应失败；旧attempt回调先经DI attempt fence，不能完成新attempt的State。

## Proof Obligations and Readiness

- PO-C1：`tests/unit-tests/core-operation-runtime.t.cpp`的C++ selector `Spec185CoreOperation`测试close/drain、
  回调退订竞态、末owner在回调释放、超时不取消业务、溢出/单reader和重复终态；TSan重复两次。
- PO-C2：`tests/installed-api/core-operation-consumer.cpp`只链接Core，使用与模型无关的字节结果/事件；
  独立头/链接与依赖闭包证明无需DI/Python/ONNX。不是另建网络协议。
- PO-C3：`tests/integration-tests/di-core-operation.t.cpp` selector `Spec185DiCoreOperation`走生产NativeInferenceClient，
  覆盖ACK关闭→提交→结果及取消、迟到回调、conversation提交失败；C++断言验证DI没有提前完成。
- PO-C4：静态依赖检查Core→DI零新增边，DI不保留第二份executor/订阅/通用完成状态实现；
  Core原stream/协作/scoped registration定向回归与受影响ABI消费者重建。仅文档检查不能关闭任务。

**Design status**: PLANNED；CB01–CB04是本轮接受的实现约束。开工前针对当时源码复核；
若既有Core组件已提供等价primitive，应复用并更新文件/签名绑定，禁止同时保留两个owner。
