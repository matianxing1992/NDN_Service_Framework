# Research: Current Multi-turn Latency

## Baseline and Evidence

2026-09-22，分支 Experimental，HEAD `b9c6930b46e2bf9d91c5ae5732403bb5728db003`，
工作树含大量未提交改动；HEAD 不能代替当前源码快照。
来源：[r260 evidence](../189-qwen-two-provider-minindn/evidence/b189-r260-runner-reuse.md)；
raw 根 `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-runner-reuse-r260/`。
读取 requester/provider logs、保存 config 及 C++ 源码；本轮不运行新模型实验。
以下“起点”是requester首个带时间戳日志，不是API submit；“checkpoint”使用导出对象的issuedAtMs，
不是文件落盘完成。旧时间口径保留，新实验须按T001真实事件口径成对重测，不能混作精确TTFT。

| Metric | r259 | r260 | Interpretation |
| --- | --- | --- | --- |
| Token counts, including EOS | 10/11/11 | 10/11/11 | 输出一致，总32；不是长序列吞吐基准 |
| Runner preparations, both Providers | 70 | 6 | 每token重载已修复；每轮仍重载 |
| Requester first log → checkpoint, s | 140.639/143.072/145.401 | 85.856/84.424/81.657 | 含启动/ACK/执行，不能称纯推理时间 |
| Selection → checkpoint, s | 78.130/80.463/82.716 | 22.880/21.284/18.847 | 合计241.309→63.011 |
| First requester log → last checkpoint, s | 514.940 | 338.405 | 三轮端到端，包含轮间间隔 |
| Resource monitoring envelope, s | 599.588 | 426.593 | 含启动/收尾，不等于生成时间 |

首轮 Provider1/0 ACK decision epoch seconds 为1790059699.899148 / 1790059699.900865；
requester ACK_CLOSED 为1790059759.770724，Selection 为1790059762.701990。
decision→close 约59.87秒，close→selection 2.931266秒。
前者不能当作 requester 验证完成时间；后者也不能在缺分段事件时全归为 planner CPU。

## Confirmed Causes

1. **Fixed collection delay**：`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py:320`
   大模型设置60000ms、小模型30000ms，经 requester 配置进入
   `NativeInferenceClient.cpp::beginCoreRequest` 调用链的 `BeginCollaborationWithProviders`。
   Core `ServiceUser::handleAckCollectionTimeout` 到期冻结集合。DI 当前传空 coverage callback；
   普通 DI options 默认5000ms，不是 Core 要求等60秒。
2. **Per-turn model preparation**：r260 每 Provider 每轮一次 RUNNER_READY，准备约2.141–4.862秒/侧。
   `OnnxRuntimeModelRunner` 构造中创建 ORT session 并真实 warmup；工厂没有跨请求 loaded-session owner。
   两侧并行耗时不能简单相加当 wall time；不能声称磁盘缓存消除了 ORT 权重/arena 内存。
3. **Buffered CLI consumption**：`examples/DI_NativeRequester.cpp` 先等待 handle 非 Pending 和 result，
   再 `handle.events().next(0)`。即使协议有流式事件，CLI 用户也不能在该循环实时看到它。
4. **Separate requester per turn**：launcher 每轮创建独立 requester 进程、重开 Runtime/PreparedModel/
   Conversation，通过 checkpoint 文件续轮；既有 KV 恢复是真的，但不是同 handle 连续对话。

## Unmeasured Boundaries

| r260 interval, seconds | Round 0 | Round 1 | Round 2 |
| --- | --- | --- | --- |
| First requester timestamp → ACK_CLOSED | 60.044 | 60.062 | 60.066 |
| ACK_CLOSED → Selection | 2.931 | 3.078 | 2.744 |
| Selection → tail first-token execution observation | 13.281 | 11.871 | 8.661 |
| Tail first → last-token execution observation | 8.191 | 8.134 | 8.315 |
| Tail last-token observation → checkpoint issuedAtMs | 1.408 | 1.279 | 1.871 |
| Checkpoint issuedAtMs → Provider TERMINAL | 29.954 | 29.934 | 29.916 |

尾节点以每epoch `EXECUTION_EVIDENCE_UPDATE`取样，排除最后checkpoint-finalize；这些事件发生在token发布前，
不是requester TTFT/到达间隔或纯ORT decode时间。两次checkpoint→下轮requester首日志为43.571/42.897秒，
其中Provider TERMINAL之后还剩13.617/12.963秒未细分。

`NativeProviderHandler.cpp::waitConversationPromotion` 在COMMIT后等待FINALIZE，
`timeoutBudget.conversationControlMs`默认30000ms；约30秒尾部等待与该边界强对应。
requester的`finalizeProviderState`发布FINALIZE且吞掉异常，没有本轮逐阶段成功记录。
尚未证明是发布失败、source提前结束、未送达还是身份拒绝；T004须先确定该首边界。
launcher先等requester exit和Provider barrier，再启动下轮，没有逐轮固定30秒sleep。

真实requester逐token TTFT/间隔、ACK认证/排队、Selection前2.93秒、轮间13秒仍须统一关联采集。
两侧Provider日志约46.62/46.13MB，failure polling有重读全文行为；未量出其时间占比。
T001/T007记录日志字节和读取成本，只有实测在关键路径才在原harness局部改为offset读取，
不单开日志平台/日志优化任务，也不先改日志级别使对照失真。

## Decisions

| Decision | Rationale | Rejected alternative |
| --- | --- | --- |
| D1: profile-level ACK1000ms | 直接删除已确认的固定空等，配置明确可回滚 | 自适应/无限加窗；提前关闭引入coverage与affinity竞态而最多再省约1秒 |
| D2: native same-handle turns + live events | 消除进程间周转并让用户真实看到首token | Python实现对话状态机、只改输出时间戳 |
| D3: bound session cache, independent KV/evidence | 减少重复加载，保留当前授权 | 整个runner连同旧plan/profile/状态直接复用 |
| D4: measure before micro-optimizing | 目前尚无证据证明decode算子是主要问题 | 先改线程数、模型精度、网络栈或planner算法 |

## Profiling and Lifecycle Constraint

`NativeRunnerPreparation.cpp` 每请求设置唯一 profile prefix 与 profileAfterRequest；
`OnnxRuntimeModelRunner.cpp` 首次请求执行后调用 `EndProfilingAllocated()`。
共享 session 后它只能由 session owner 结束一次。采用独立 load provenance 与每请求实际
执行观察，旧 profile 的 request/attempt 保持原值；不把它当当前请求的新 profile。
详见 [CD-04](contracts/design.md#cd-04-resident-cpu-session)。CUDA 路径不改。
当前 Provider stop 不等价于 worker drain；关闭缓存前停止准入、收束所有持有者，最后释放 owner。

## Scope Audit Decisions

保留已有效的 Spec189 r260 request-local runner reuse、r259 affinity 和 KV校验，不重做。
不把 GPU、Repo full-path、SIF、泛化自适应 scheduler、聊天 UI 混入性能计划。
核心性能任务自带安全、负例、C++ oracle、文档义务，不按“写测试/实现/审查/记报告”机械拆任务。
