# Feature Specification: Multi-turn Token Generation Latency

**Feature Branch**: `Experimental`
**Created**: 2026-09-22
**Status**: Draft
**Input**: 分析多轮生成耗时异常，制定有细节的 Spec190，并审计遗漏和非核心任务；优先验证 1 秒 ACK 收集窗口。

## Purpose and Scope

唯一目标：降低 `MiniNDN + Qwen3-0.6B + two Providers + native NDNSF-DI`
多轮对话从提交到首 token、后续 token 和下一轮可开始的等待，保持正确输出和 KV 复用。
以 Spec189 r260 的缓存兼容执行为性能基线；不把该范围升级为完整 Repo 获取资格。
本次交付是计划和审计，不是产品实现。Spec189 的未完成资格不自动关闭。

纳入：ACK 窗口与软件排队、实时 token 消费、同一原生 Conversation 连续请求、
轮次终态/退出等待、短期 CPU 模型 session 驻留及其必要的证据/资源边界。
排除：Repo 重构、加密协议重做、GPU/CUDA 新能力、跨机 KV 搬运、SIF/Tiger、量化、
更换模型、通用调度重写、聊天 UI、无限性能调参和恢复 Spec189 全部遗留任务。
不新增自适应 ACK 窗口或提前关闭策略：先用已有固定窗口机制完成最小改进。

## User Scenarios & Testing

### User Story 1 - Bounded Request Admission (Priority: P1)

用户发出一轮请求后，不应在候选已响应时固定空等一分钟。
**Why this priority**: r260 首轮 Provider ACK 决策到 ACK_CLOSED 约 59.87 秒，是已定位的大头。
**Independent Test**: 配置窗口 1000ms，真实验证候选后选择；记录发布、接收、验证、关闭及规划耗时。
**Acceptance Scenarios**:
1. **Given** 两个已就绪 Provider，**When** 请求发布，**Then** 使用 1000ms 有界窗口，不继承模型大小分支的 60000ms。
2. **Given** 无效、重复、迟到或缺角色 ACK，**When** 窗口关闭，**Then** 不放宽验证或复活冻结集合，明确报告缺失阶段。
3. **Given** ACK 处理超过窗口，**When** 请求失败，**Then** 保留原始证据，定位发布/传输/认证/排队；禁止自动延长掩盖问题。

### User Story 2 - Immediate Tokens and Continuous Turns (Priority: P1)

用户在本轮完成前就能看到 token；正常终态完成后直接在同一 Conversation 发下一轮，
无需每轮退出进程、重建 Runtime、重开模型及回读文件来模拟对话。
**Why this priority**: 当前 CLI 先等 result，再读取 events；当前实验每轮另启 requester。
**Independent Test**: 同一 C++ Runtime/PreparedModel/Conversation 三轮顺序执行，事件实时产生且后两轮使用父 KV。
**Acceptance Scenarios**:
1. **Given** 流式请求，**When** 首 token 已可读而结果尚未完成，**Then** CLI 输出并 flush 当前 token 事件。
2. **Given** EOS/EOT 或 token 上限，**When** 本轮 checkpoint 与必要 receipts 提交，**Then** 下一轮可开始；不得等待无关周期定时器。
3. **Given** 取消、缺 receipt、迟到回调或紧接 result 的下一次 request，**Then** 无双重终态、无陈旧 KV 提交、无错误 busy 竞态。

### User Story 3 - Safe Resident Model Reuse (Priority: P2)

同一 Provider 短期连续使用相同模型角色时复用已加载 CPU session，仍重新验证本次授权和 Selection。
**Why this priority**: r260 已消除每 token 重载，但每轮每 Provider 仍加载/预热一次。
**Independent Test**: 两个独立合法请求使用相同加载身份只构造一次 ORT session；不同身份 miss，退出后 owner 全释放。
**Acceptance Scenarios**:
1. **Given** 相同不可变模型及运行契约、有效授权，**When** 下一轮请求到来，**Then** 复用 session，但新建请求证据和可变状态包装。
2. **Given** 模型变化、过期、显式驱逐或关闭，**Then** 不再新租用旧项，活跃使用结束后释放；不得造成悬空 backing 或无界保留。
3. **Given** cache hit，**Then** 实际调用 ORT，不能复用旧输出或把旧 profile 标成当前请求。

## Acceptance Evidence Contract

所有 `spec190-latency-tests` 及其 selector 均为 **planned**，由 tasks.md 指定任务在 Waf 注册；不是已有测试。

| Story / FR | Production entry / callers | Observable outcome | Independent oracle / C++ selector | Negative / recovery boundary | Evidence owner / Batch |
| --- | --- | --- | --- | --- | --- |
| US1 / FR-001–003 | DI_NativeRequester → NativeInferenceClient → ServiceUser | 有界关闭及分段延迟 | spec190-latency-tests / AckWindow, PhaseTiming | 无效/迟到/认证在途/取消 | Core+DI / B190-01,02 |
| US2 / FR-004–006 | Conversation::request, EventReader, Runtime::close/drain | 实时事件、同 handle 续轮、及时终态 | spec190-latency-tests / LiveTurns, TerminalDrain | 缺 receipt、busy、事件乱序、失败退出 | DI+Core / B190-03,04 |
| US3 / FR-007–009 | NativeProviderHandler → RegistryNativeModelRunnerFactory → ORT | load count、租约释放、新请求证据 | spec190-latency-tests / ResidentSession | 错身份/失效授权/驱逐/关闭/加载失败 | DI / B190-05 |
| all / FR-010–012 | installed requester/providers + maintained MiniNDN launcher | 正确 token、耗时和资源结果 | installed C++ oracle + named native regressions | 假 hit、错候选、失败不能剔除 | B190-06,07 |

### Edge Cases

窗口与认证完成同刻、重复 ACK、网络丢包/抖动、Provider 冷启动未 ready、授权失效、
父 KV 节点失联或过期、前轮失败、两会话并发、session 在用时驱逐、退出时加载尚未结束、
profiling 已结束、模型外部权重仍有 owner、RSS 未立即下降但对象已经销毁。

## Requirements

### Functional Requirements

- **FR-001**: Phase Timing — MUST 分开记录发布、ACK 到达/验证/关闭、规划、Selection、加载、prefill、decode、token 交付、checkpoint、drain；不可跨机器直接相减不一致时钟。
- **FR-002**: One-second Collection — MUST 将本 Qwen 多轮 profile 的默认 ACK 窗口设为 1000ms，保留显式正值覆盖、打印实际生效值；不修改无关 Core 服务默认值。
- **FR-003**: Fail-closed Admission — MUST 保留全部 ACK/offer/授权/placement 校验；截止后不吸收迟到候选，认证在途行为有界且可测，不自动扩大窗口。
- **FR-004**: Live Token Delivery — MUST 在本轮 terminal 之前消费并输出 token；区分 token 首达和最终文本，EOS/EOT/预算语义不改。
- **FR-005**: Persistent Conversation — MUST 真实复用同一 C++ Runtime、PreparedModel、Conversation 完成至少三轮，保留当前 affinity 与 KV 身份验证，Python 不实现轮次状态机。
- **FR-006**: Event-driven Completion — MUST 在必要 receipts/commit/worker 收束完成时推进终态和退出，不用固定 sleep 替代完成条件；超时仍为上限，不能提前丢弃服务或回调。
- **FR-007**: Exact Session Identity — MUST 按不可变模型/外部权重摘要、role、完整 IO/KV/position 契约、backend ABI、实际 device 与安全域/epoch 复用；path、模型名或上次选择不能单独作为命中依据。
- **FR-008**: Bounded Ownership — MUST 将模型缓存与请求/KV owner 分开，明确 idle TTL、容量、在用租约、替换、失效、异常和 shutdown；缓存不能持有旧授权作新请求凭证。
- **FR-009**: Honest Reuse Evidence — MUST 区分 session 加载级 profile 与当前 request 实际执行记录；不得重新命名旧 profile、重复 EndProfiling 或将命中当作执行完成。
- **FR-010**: Matched Validation — MUST 固定模型/token 输入/采样/拓扑/资源/日志级别，保留对照和全部失败；正确输出、KV、cleanup 与性能同时通过才可宣称改进。
- **FR-011**: Native Acceptance — MUST 由生产 C++ target、C++ fixture/assertion/oracle 证明行为；先实现与静态审查、再定向测试、收敛审计后运行 MiniNDN。
- **FR-012**: Immutable Scope — MUST 绑定源码含未跟踪文件、安装产物/依赖、harness、配置、模型及 oracle 身份；复用并补足既有 preflight，拒绝候选时零启动副作用，历史 raw 不覆盖。

### Key Entities

TurnTiming、AckWindowPolicy、LoadedSessionIdentity、SessionLease、LoadEvidence、RequestExecutionObservation；见 [data-model.md](data-model.md)。

## Success Criteria

### Measurable Outcomes

- **SC-001**: Bounded Admission — 健康、预就绪的本地两节点 profile 使用 1000ms 窗口；C++ 可控时钟验证 deadline，实机逐轮报告发布到 ACK_CLOSED 及超调，正常轮不超过 1500ms；异常不以延长配置通过。
- **SC-002**: Visible Streaming — 多 token 轮的首 token 在终态前交付，序列与原始 tokenIds 一致；没有“完成后一次性打印”的伪流式。
- **SC-003**: Faster Conversation — 匹配对照的三轮 conversation 首次提交到最后 checkpoint 的中位耗时至少降低 50%；同时报告 TTFT、decode 间隔、轮间空隙，不以仅缩短收尾冒充 decode 加速。
- **SC-004**: Reuse and Correctness — 同 handle 三轮 EOS/EOT/预算正确；固定 r260 样本维持 10/11/11 token（含 EOS），两侧后续 KV 命中；resident enabled 时每 Provider 相同角色合计一次 load，禁用对照三次。
- **SC-005**: Bounded Resources — TTL/驱逐/shutdown C++ 析构计数与 lease 归零，实验无遗留进程或新增整模型副本；健康轮durable checkpoint commit→Provider TERMINAL≤2秒，失联补偿预算不缩短；报告 RSS/可用内存/swap，高水位不可由仅清空 map 证明下降。
- **SC-006**: Reproducible Improvement — 至少三组配对实验，报告每组成功/失败及所有样本，至少60秒累计真实请求观察；不通过 sleep 凑时间。主decode指标为requester相邻生成token接收时间差，排除首token、EOS/EOT及checkpoint-finalize；先每轮求中位、再按相同轮号跨配对组比较，处理组不得劣于对照10%以上。ORT Run时间另列，不与接收间隔混比；小样本p95仅描述，不作总体保证。

## Assumptions

1 秒是本 workload 的目标窗口，不是“全球任何网络 RTT 必定小于200ms”的保证。
ACK 涉及路由、进程队列、密钥/签名验证，不只往返网络传播。预热/授权 readiness 在开始请求前单列，不能挪动请求内工作美化计时。
初版 resident 范围限实际 CPU、现有不可变明文 assembled cache；加密临时 backing 或 CUDA 不满足复用前提时走原路径并记录 bypass，不扩展新能力。
`Spec189 = correctness/full-path obligations`；`Spec190 = matched multi-turn latency`。两者资格和原始证据分别保留。
