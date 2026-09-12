# C-02 Preparation, Cache and Identity

## Preparation Key and Trust

准备 key 使用带版本域 `ndnsf-di-preparation-v1` 的规范化编码和 SHA-256：
用户安全域（principal + 配置的 trust domain）、完整 NativeModelDescriptor、taskName、
完整NativeRequestContract规范摘要（含service/composition/task descriptor/tokenizer/generation mode）、inputLayoutDigest、
canonical catalog configuration digest、adapter descriptor identity、splitter identity、
canonical source/initializer 内容摘要。canonical graph 与 planning graph 保持独立字段。
catalog 中包含 recipe/ABI/profile 等字段，因此其规范摘要变化必须产生新 key。
本期允许把现有 catalog 中 protection epoch 纳入完整 config digest，导致保守 cache miss；
不能为提高命中率擅自删除当前安全字段。未来 metadata/加密 publication 分层优化另审。

metadata digest 只能证明内容匹配，不能证明发布者可信。本期复用 operator-pinned 配置入口，
网络 Data 使用既有验证/授权的 Repo fetch；不接受未认证的远程 catalog 自我声明。
不同主体不共享 READY/in-flight 索引，防止命中时间和明文包越权泄露。
本地路径是 locator，不是身份；读入 owned bytes 后验证 digest，后续路径改写不改变 Package。
绝不把 role stage manifest 或 JSON 元数据当 canonical ONNX source。

## Cache Policy Matrix

| Policy | READY exists | Same-key job only | Neither |
| --- | --- | --- | --- |
| RequireReady | 返回命中 | 立即 MODEL_NOT_READY | 立即 MODEL_NOT_READY |
| UseOrWait | 返回命中 | 加入，等待自己的 deadline | 立即 PREPARATION_NOT_IN_FLIGHT |
| UseOrFetch (default) | 返回命中 | 加入 | 启动一次 fetch/verify/inspect |
| Refresh | 创建/加入 refresh generation；不返回旧命中 | 加入 refresh generation；普通 job 不被替换 | 创建 refresh generation |

Refresh 对同一不可变身份重新获取和验证，不解析 mutable “latest” 到新 revision。
旧 READY/lease 在刷新失败时仍可用；成功原子切换未来 lookup 的 generation，已有 PreparedModel
继续绑定旧 generation。每 key 最多一个普通 job 和一个 refresh job，禁止旧 job 覆盖新 refresh
generation。完成顺序通过 generation CAS 控制；refresh 先完成时旧普通 job 只唤醒自己的等待者。

## Work and Cancellation

状态为 ABSENT → PREPARING → READY；失败 job 移除并唤醒等待者，不长期缓存失败。
job 有 RuntimeConfig.preparationJobTimeout 的独立 steady_clock deadline；单个等待者期限
不能延长或缩短共享 job。普通API仅通过PreparationHandle.cancel设置native取消状态，最后用户handle释放同样取消该waiter；
内部保留控制端口，旧兼容cancelled回调须thread-safe、非阻塞，异常视为该waiter取消。
取消检查间隔目标上限50ms（调度延迟另计），禁止忙等；resultAsync局部等待超时不等于waiter超时，见C-07。
最后一个等待者退出时取消尚未发布的 job；READY 发布与取消在一个 owner 提交点裁决。
超时/取消已经返回的等待者不能稍后收到成功对象；其他有效等待者正常完成。

准备顺序：验证参数/可信配置 → 得出 key → 查 READY/job → 有界取得 owned source/initializer →
内容与长度校验 → NativeRequestCatalog::load/已有 canonical inspection → 校验 model/task/
双 graph identity/adapter/splitter → 建立不可变 Package → 一次提交 READY。
文件路径 source 与配置 exact Data name 必须绑定同样内容；网络失败不退回未验证本地文件。
复用现有 C++ catalog 和 adapter；准备中不发布 request-specific artifacts、不请求 grant、不选 Provider。

## Package and Leases

Package 持有 `NativeRequestCatalog` 的 inspected model、preparation catalog、splitter、state mapping，
以及完整taskContract及其规范摘要/inputLayoutDigest（C-01），不能仅用taskName绑定执行语义；
以及 source owned bytes/原有存储 lease、默认 placement 和安全域。只公开 C-01 的不可变视图。
目录内存/源字节计入 maxPreparedBytes；lease 包含内存和磁盘 source 生命周期，不能只 pin map entry。
缓存 LRU 仅驱逐无外部 lease 的 READY；请求、会话及 runner 各自持有其所需 lease。
超过单包或总预算且无可驱逐对象则 CACHE_BUDGET_EXCEEDED；下载先限制 source max bytes，
暂存与 READY 总量受预算约束。没有隐藏无限暂存，也不为了腾空间破坏 active lease。

缓存不存 request/attempt、admitted offers、grant、Selection、plan 或可执行会话 KV。
splitter 与结构目录可固定；候选 enumerate 使用本次 PlanningBudget，placement 仍在 ACK_CLOSED 后。
prepare 后 input schema 校验/encode 每次都执行；不能用先前输入产生的 NativePreparedInput 作为缓存。

## Counterexamples / T003,T004

8 并发相同 key、不同主体相同模型、相同路径字节变化、错 source/initializer digest、Qwen 逻辑 graph
与 canonical graph 混用、缓存 miss 无 job 的 UseOrWait、单 waiter 取消、最后 waiter 取消、
refresh 失败保留旧 lease、refresh/普通 job 逆序完成、预算满且全 pin、读取中 close、非 ONNX source。
oracle 检查生产 fetch/inspection 次数和 READY generation，不在 fixture 重写生产 hash 算法作为唯一判据。
