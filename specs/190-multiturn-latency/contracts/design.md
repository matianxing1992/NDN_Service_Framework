# Design Binding and Validation Contract

**Status**: proposed；以下新增签名/selector 尚未实现。G1–G6、C1–C3、D1–D4、B1–B3、M1–M3适用。
新增stage传输与per-node Repo复用由[CD-06–09](material-reuse.md)控制；T011完成前protected缓存限制仍保留。
路径缩写 `DI/` 指 `NDNSF-DistributedInference/cpp/ndnsf-di/`；`ORT/` 指 `NDNSF-DistributedInference/cpp/adapters/onnx/`。

## CD-01 Timing and ACK Window

**Class delta**：复用 Core `ServiceUser` 的 ACK collection、DI `NativeInferenceClient` 阶段、
`RuntimeTiming` 日志；只增结构化事件，不增第二个状态机。
**FN before/after**：`BeginCollaborationWithProviders(...)` 和 request 公共签名不变；
`NativeRequestOptions::ackTimeoutMs` 通用默认不改。launcher 为 Qwen profile 增加
`--ack-timeout-ms`（default1000）；明确覆盖写入原 `request.ack_timeout_ms`，requester打印生效值。
**FIELD ownership**：Core拥有发布/接收/认证/冻结时间，DI拥有规划与模型阶段，CLI拥有交付时间。
**FLOW**：ready→publish→fixed deadline→原验证及快照→DI offer admission/placement→Selection。
不注册新 coverage callback；Core不理解模型层或KV。认证仍在途的既有短延迟须计入close overshoot，
不得当已验证候选使用，不偷偷把1000改成60000。超过目标时T002定位首个软件边界再修。
**PO**：AckWindow/PhaseTiming；可控时钟覆盖999/1000/1001ms、无候选、重复、验签失败、deadline/取消同刻。

## CD-02 Live Native Conversation

**Class delta**：`examples/DI_NativeRequester.cpp` 增原生 multi-turn driver，沿用
`DI/Conversation.cpp`, `PreparedModel.cpp` 及 `PreparedModel.hpp` 中RequestHandle/EventReader的公开对象。
**FN before/after**：`RequestHandle Conversation::request(const Input&, const RequestOptions& = {}) const`、`RequestHandle::events/result`
签名保持；新增CLI config `turns` 数组，每项 input_file/options_file/output_file，原单轮参数保持兼容，
首轮完整输入、后轮增量输入由C++依次提交。一个Runtime、一次prepare、一个Conversation；
不每轮重建Conversation，但每轮产生新的RequestHandle/requestId。
**FLOW**：request→边读取EventReader边输出/flush→terminal event→result→checkpoint→下一轮。
阻塞读不能占Face/operation执行线程；回调只排队。token去重/次序/终态按既有身份执行。
**Failure**：任意轮失败停止后续轮，原始错误及checkpoint保留；cancel先完成本轮收束再close。
关闭只在全部轮结束/失败后一次进行。检查 `Conversation::State::active` 清理与 result可见性，
成功result后顺序request不得偶发busy；真正并发request仍拒绝。
**PO**：LiveTurns C++ fixture证明首事件早于terminal、第二轮真实复用同对象、失败不继续、立即续轮、真实busy。
另用C++父进程/pipe驱动真实CLI，暂停后续生成时已读到首事件，以证明实际stdout flush；
暂时没有事件不当EOF，失败/取消缺正常terminal时仍按请求状态有界结束。
`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` 只生成输入表、启动进程、监控资源；不持有对话状态逻辑。

## CD-03 Terminal and Drain

**Class delta**：检查 `DI/NativeInferenceClient.cpp::commitConversationTurn/collectConversationReceipts/close/drain`、
`publishConversationControls/finalizeProviderState`、`DI/NativeProviderHandler.cpp::waitConversationPromotion`、
`DI/Runtime.cpp::close/drain`、Core `OperationRuntime.cpp`、`ServiceUser` 的 pending timers 与终态服务owner。
**FN before/after**：公开close/drain签名及“close后拒绝新操作，drain(timeout)返回是否收束”不改。
不得把timeout设0或忽略drain返回值来消除等待。
**FIELD/FLOW**：request任务/定时器 owner 与 retained immutable serving owner 分开；只取消本操作已无消费者的timer，
必要receipt和协议服务存活到明确终态。顺序保持staged receipt→COMMIT→verified commit ACK→durable journal→FINALIZE；
FINALIZE仅结束补偿等待，不作为新的checkpoint提交。丢FINALIZE不能回滚已持久提交的KV；
未commit的超时才回滚。记录FINALIZE publish/receive/validate/accept/error，不能继续无诊断吞异常。
两角色仅一侧COMMIT或commit ACK丢失时，不得发布成功checkpoint或开始下一轮；已提交侧走既有补偿，
迟到/重复控制不复活失败轮。持久提交成功后丢FINALIZE属于不同边界，不能误回滚。
completion通知后无活跃业务待办时立即唤醒drain。
**Diagnosis gate**：T004先用T001事件定位控制交付/接受的首个真实边界；如果同handle已消除轮间成本，
生产逻辑保持不变，任务以原生负例与实测无多余等待关闭，不为“优化”强造修改。
不预先引入新FINALIZE握手协议；若确证既有交付契约不足，先在本CD冻结最小应用层确认字段、
旧版本/丢失处理与owner，再修对应task，不能由编码者即兴扩展Core wire。
**PO**：TerminalDrain：完成、缺receipt、延迟callback、cancel/close竞态、正常/超时drain及外部Face先后析构。

## CD-04 Resident CPU Session

**Class delta**：在 `ORT/OnnxRuntimeModelRunner.cpp` 将 `Impl` 的加载对象分离；新建
`ORT/OnnxRuntimeSessionCache.hpp/.cpp`，Provider host持有，不用process-global singleton，
不修改只缓存immutable metadata的 `ProviderArtifactCache` 语义。
**FN before/after**：保留 `OnnxRuntimeModelRunner(NativeModelRunnerSpec)` 和
`registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory&)` 原入口；新增接受
`std::shared_ptr<OnnxRuntimeSessionCache>` 的重载。新增cache接口：
`explicit OnnxRuntimeSessionCache(std::chrono::milliseconds idleTtl, std::size_t maxEntries)`；
`bool evict(const std::string& identityDigest)`；`void evictIdle()`；`void close() noexcept`；
`bool drain(std::chrono::milliseconds timeout)`。
内部获取 `acquire(identity, loader, deadline, cancelled)` 返回RAII lease，类型隐藏在adapter内部。
同key single-flight，无mutex内加载；活动lease pin住session/backing。idle sweep由cache拥有的可唤醒线程/
定时owner执行，析构停止并join；idle120秒从最后lease释放计。close不等待时由drain显式报告尚有lease，
缓存被销毁后活跃lease仍可安全释放，但Provider正常退出须先drain所有worker再断开依赖。
首次加载由cache task owner持有已验证immutable输入及backing，不引用发起请求ctx/grant；
发起者取消仅撤销其等待/lease，其余已授权waiter可在自己的deadline内接收结果。
所有waiter取消时不强行终止不支持取消的ORT构造，但禁止向已close/retiring项发布；
构造完成立即释放无人需要的结果并唤醒drain。close后迟到load不得重新入缓存。
`evict(key)`不存在返回false，存在返回true并原子标Retiring，立即拒绝新lease，最后活跃lease释放后销毁；
`evictIdle()`仅retire无活跃lease条目。`close()`幂等地关闭准入并retire所有条目，不取消已经获准的run。
close之后acquire报明确closed错误，不能偷偷退回新建session绕过关闭；drain超时返回false，不能报告全释放。
**Identity**：遵守[data model](../data-model.md)，只允许可信preparation验证出的digest和contract；缺字段fail-safe miss，
相同path但内容改变不能命中。assembler已有稳定assembled digest须复用，不每轮复制模型。
T005的protected临时明文backing/CUDA先bypass并记录原因；T011单独闭合真实protected复用，
未通过不得宣称Repo热缓存路径PASS。不得让cache强持有上次grant/context。
**Owner flow**：NativeProviderHandler当前授权/Selection/spec校验→factory→cache acquire→fresh wrapper→
每epoch真实run；请求结束释放wrapper lease，KV按原store保留；host退出stop admission→worker drain→cache close/drain。
host `examples/DI_NativeProviderExecutable.cpp` 显式注入并收束；库 `DI/Provider.cpp` 采用同一owner契约，
无应用私有第二实现。默认1slot/idle120秒仅适用于本Spec190 CPU多轮profile，显式配置启用resident；
原无cache参数的register/构造入口及其他profile保持原行为，库Provider从同一显式配置注入owner。
本profile可显式禁用作同构对照，不把1slot强加给已有多角色/多模型应用，不扩展多模型调度。
**Evidence**：`DI/ExecutionEvidence.hpp/.cpp` 增可选loadId/cacheHit/load-evidence引用；session owner只结束profile一次。
复制的只是明确标为load provenance的不可变数据；当前plan/request/attempt/实际run计数来自新wrapper。
原profileRequestId/Attempt保持原值或不作当前请求profile输出，禁止改写为本轮。
新增C++ oracle必须验证load事件与当前真实run链；旧要求逐请求profile的资格入口不自动升级，CUDA仍走原路径。
**PO**：ResidentSession 用C++生成小型多层ONNX、真实ORT两请求、精确输出、独立加载计数及析构探针；
fixture RAII删除全部生成文件，失败也清理。不读取Qwen作单元fixture，不以假runner证明ORT加载复用。
覆盖key单字段变化、外部权重、非法摘要、并发同key、失败重试、取消waiter、busy替换、TTL边界、explicit evict、
close在load/run中、失效权限拒绝与旧profile污染反例；并发evict/acquire、evict在用项后新租用拒绝、
closed后acquire拒绝、drain超时和最终成功均验证，保留原无cache入口行为的回归。

## CD-05 Qualification and Migration

不新增第二套模型harness；在既有launcher中接线同handle C++ driver，现有单轮模式仍可显式用于对照。
在 `examples/Spec189TwoProviderOracle.cpp` 共享现有协议断言，新增Spec190 timing/residency检查模式或
薄C++入口，target `spec190-multiturn-oracle`（planned），不复制一份独立协议规则。
`tests/wscript` 新target `spec190-latency-tests`（planned）链接真实Core/DI生产库；对应fixture文件
`tests/unit-tests/spec190-{timing,ack-window,live-turns,terminal-drain,resident-session}.t.cpp`。
`examples/wscript` 注册并安装新oracle；若跨库新符号，记录definition TU→target及 `nm -C`/`readelf`闭包。
现有spec189-logits-dtype、spec189-epoch-projection回归保留；Python只检查启动/配置，不作为模型行为判据。

三组配对：相同候选支持control（ACK60000、原单轮进程、resident disabled）与treatment
（ACK1000、同handle、resident enabled）；轮次token/采样/模型/拓扑/资源一致，交替顺序，固定输入。
新增Repo-enabled的对照与处理必须使用相同合法warm材料状态，不能与旧cache-compatible数据混比；
冷入库/三次重启/缺层场景另外按CD-06–09执行，不为每run复制一个大Repo目录。
报告每组结果、首轮cold与后轮warm，不能从3样本承诺全球p95。必要时一次消融只改ACK以确认主要因果。
TTFT从request提交到首token交付；decode间隔分离first token/prefill；总耗时从第一次提交到最后checkpoint，
启动/最终退出另表，所有失败计入成功率。后轮parent/role/provider/boot/epoch必须与有效KV receipt匹配。
不更改EOS/1024 token预算来制造吞吐差异；初版只是固定短对话延迟验收，不宣称长序列吞吐结论。

源/库/harness/配置/model/oracle变更分别使受影响静态门、构建、安装、preflight和实验失效；
只改说明文档不重跑模型。完整候选元组冻结，单candidate/gate仅一个active subject。
preflight mutation：错binary hash、模型digest、配置ACK值/输入引用、旧oracle和缺依赖均在任何模型/进程启动前拒绝。
原始日志、模型、密钥不进Git。双PDF与中文API在实施改变公开行为的对应任务同步；本次规划不把proposed写成current。
