# ICNP 2026 NDNSF May-20 快照—评审—Spec 163 追踪矩阵

## 1. 使用规则

本文档用于隔离两个不同的研究对象：

1. **ICNP 2026 #979 所评审的原论文/May-20 NDNSF**；
2. **2026-07-29 工作树中的 Spec 163 分布式推理协作机制**。

除非一项能力能够在 May-20 Git 对象中直接定位，否则不得把它写成原论文已经实现或评测的能力。Spec 163 只能作为投稿之后的新设计和新证据，不能反向修补原论文的事实陈述。

状态含义：

- `FULLY_ADDRESSED`：Spec 163 已有实现和直接证据，足以回答对应问题；
- `PARTIALLY_ADDRESSED`：已有机制或有限证据，但评审要求的尺度、对照或理论仍不完整；
- `NOT_ADDRESSED`：Spec 163 没有针对该问题提供直接证据；
- `MADE_WORSE`：新机制扩大了问题且没有补偿证据。

## 2. 快照身份

### 2.1 评审材料

| 对象 | 身份 |
|---|---|
| 评审邮件 | ICNP 2026 submission #979，标题为 *NDNSF: A Framework for Secure and Reliable Service-Oriented Applications over NDN* |
| 评审邮件本地副本 | `/home/tianxing/.codex/attachments/b9f83a0d-e457-4969-a2e8-ca45fbcee717/pasted-text.txt` |
| 评审邮件 SHA-256 | `85b4b55ed4abb10a997701d4886102f04565771e7fb73da08c024fe47a39b072` |
| 本地候选论文 PDF | `docs/PAPER/reference-pdfs/Named_Data_Network_Service_Framework (2).pdf` |
| PDF SHA-256 | `0eb971762e7e76f055411fe29ae7ad440a09c6087e56d5784c5c46595d40123e` |
| PDF 内部标题 | *NDNSF: a Distributed and Secure Service Framework for NDN* |
| PDF 元数据 | 2026-05-15 13:33:15 CDT；11 页；809454 bytes |

本地 PDF 的内容与评审所描述的 one-to-many、NAC-ABE、selective ACK、NSC/gRPC 实验高度吻合，但其内部标题与 HotCRP 投稿标题不同，且 PDF 未嵌入 Git commit。故它是**高相关候选稿件**，不是已经密码学证明的 HotCRP 上传文件。

### 2.2 May-20 主仓库快照

| 字段 | 精确值 |
|---|---|
| annotated tag | `v0.1` |
| tag object | `9ebf9a4167373d8646ccb59c0f9f0287da557972` |
| commit | `be9421f2db8499ba8a42ad1aed2cb42c3f0d81b6` |
| root tree | `c9cc0c51ba4a74b855ed9eec84f5459260b12ee1` |
| parent | `e397f99de6a60f66c83415c2266b656a13485617` |
| author/committer time | `2026-05-20T01:17:48-05:00` |
| subject | `Release NDNSF runtime v0.1` |

这是目前仓库中最强的、可复现的 May-20 运行时身份，因此本矩阵将它定义为 **canonical May-20 runtime snapshot**。

同日后续 commit `d42e6f8f66d4b38310647725aeb7544086f99b2d`（tree `2006af686405290a8df72f91dfe7b48f7eb9b09b`，`2026-05-20T03:04:35-05:00`，*Default NDNSF to async SVS publishing*）是 `v0.1` 的后继，不属于该 release tag，故排除。5 月 21 日之后的统一 API、discovery opt-in 等 commit 同样排除。

绑定强度必须分开表述：

- **主仓库 release 身份：高置信**。commit 和 tree 可由 Git 对象直接验证。
- **论文 PDF ↔ `v0.1` 一一对应：中等置信**。日期、内容和 release tag 一致，但缺少 HotCRP 上传包中的 commit manifest。
- **完整多仓库构建身份：不完整**。tag message 记录 `ndn-svs` 为 `3fd9b53`，但 `ndn-cxx f99dc2ae` 和 `NAC-ABE c56b70d` 均标记为 `dirty`；未保存的依赖补丁无法从主仓库 tree 恢复。

因此，准确表述应为：

> 原论文对应的可复现主仓库快照采用 NDNSF `v0.1`（commit `be9421f…`，tree `c9cc0c…`）。现有材料尚不能证明 HotCRP PDF 与该 tree 的一一绑定，也不能完全重建当时两个 dirty 依赖的源码状态。

## 3. May-20 能力边界

对 `v0.1` Git tree 的核验表明，May-20 已包含：

- `AckDecision`、selective ACK 和自定义选择；
- user-side adaptive admission control；
- provider-side adaptive ACK admission；
- `UserToken` / `ProviderToken` 握手和相关校验；
- NAC-ABE 权限路径以及 MiniNDN/真实环境实验脚本；
- 1–10 providers 的实验参数和带 loss topology 的若干实验入口。

May-20 tree 中没有发现以下 Spec 163 标识或目录：

- `begin_collaboration`、`ACK_CLOSED`、`commit_plan`；
- `PreSplitFirstStrategy`、`ExecutionLease`、`GenericSelection`；
- `NDNSF-DistributedInference/`、`pythonWrapper/`；
- 分布式推理 DAG、multi-role placement、model-shard retention。

这意味着“adaptive admission/selective ACK”可以属于原论文，“协作计划冻结、角色放置、分布式推理恢复”不能属于原论文。

## 4. 追踪矩阵

| ID | 评审问题 | May-20 快照事实 | Spec 163 改进 | 状态 | 仍缺证据 |
|---|---|---|---|---|---|
| NOV-1 | 979B/979E：创新主要是已有 NDN 组件的系统集成，科学新颖性不清 | `v0.1` 的核心是 Sync、NAC-ABE、admission、selective ACK 和 provider selection 的集成 | 新增通用的 `begin_collaboration → ACK_CLOSED → commit_plan` 边界、不可变候选集、精确多角色 Selection、签名 offer、DAG/lease/compensation 和可替换 placement strategy | `PARTIALLY_ADDRESSED` | 需要与现有协作推理、workflow、RPC/edge orchestration 文献作逐项机制比较，并把“新协议抽象”与“工程组合”分开；不能只凭代码规模宣称创新 |
| NOV-2 | 979B：缺少 admission stability 和 provider-selection optimality 分析 | May-20 有 adaptive admission 和启发式选择，但没有稳定性/最优性论证 | `PreSplitFirstStrategy` 将容量、resident state、等待时间、RTT、带宽等纳入确定性可替换策略，并验证计划可行性 | `PARTIALLY_ADDRESSED` | 没有稳定性证明、近似界、最优性基线或策略 regret；当前 audit 明确不声称 universal optimizer quality |
| REL-1 | 979E：可靠性提升的因果来源不清，缺少机制消融 | May-20 同时启用多 provider、admission、ACK、选择与安全，难以归因 | Spec 163 将 plan freeze、selection gating、lease、attempt fencing、bounded recovery 分成可检查边界，并执行故障/顺序矩阵 | `PARTIALLY_ADDRESSED` | 需要逐组件 ablation：固定单 provider、仅多 provider、无 lease、无 preparation reuse、不同 recovery；报告 success/latency/overhead 的因果差异 |
| CONC-1 | 979C：多个用户同时请求同一 provider 的语义和并发支持不清 | `v0.1` 有 outstanding-window 和 provider ACK admission，但不等于高并发资源隔离 | Spec 163 增加精确 role-provider assignment、provider-local GPU admission ledger、execution lease、attempt fencing 和 async preparation | `PARTIALLY_ADDRESSED` | 需要多用户×多 provider 并发实验、队列公平性、吞吐、tail latency、GPU/CPU/内存利用率和饥饿检查；bounded histories 不是任意调度证明 |
| SEL-1 | 979C/979D：未选择 provider 的行为、all-responders 拥塞和 provider 资源优化不清 | May-20 已有 Selection 和 selective ACK，但论文解释/负载证据不足 | Spec 163 用 selection-gated provider、exact assignment 和 plan digest 将“谁可以执行哪个 role”固定下来 | `PARTIALLY_ADDRESSED` | 需要网络拥塞实验、错误/恶意额外响应测试、provider utilization 与 selection policy 的联合测量 |
| BASE-1 | 979A/979C：gRPC 被固定到单 endpoint，缺少客户端 failover，比较偏置 | `v0.1` 的 `greeter_client.py` 打开固定 `10.0.0.58:50051` channel；未看到 round-robin/failover policy | Spec 163 没有重做 gRPC baseline | `NOT_ADDRESSED` | 必须实现公平的 gRPC 多 endpoint/failover/load-balancing baseline，匹配健康检查、重试预算、连接预热和资源数量 |
| BASE-2 | 979B：只比较 NSC/gRPC，缺少其他 RPC、pub/sub、edge framework | May-20 baseline 范围有限 | Spec 163 引入 DI 场景，但没有新增外部框架对照 | `NOT_ADDRESSED` | 需要选择并解释至少一类协作推理/edge orchestration baseline；对不能公平复现者给出排除标准 |
| SEC-1 | 979A/979B：NAC-ABE 高并发开销、认证延迟、key distribution cost 未量化 | May-20 有 NAC-ABE 功能和 token 安全路径，但没有对应尺度实验 | Spec 163 保留签名、加密、token/selection 边界，并在 MiniNDN 载体上跑通功能门 | `NOT_ADDRESSED` | 功能通过不等于性能证据；仍需并发用户、属性数量、policy size 下的 encrypt/decrypt、key fetch、auth latency、CPU/内存和吞吐曲线 |
| SEC-2 | 979D：属性增长、授权/撤销、policy change、key rotation/cache 语义缺失 | May-20 有属性路由和 controller 权限分发，但未给出动态生命周期证据 | Spec 163 的 digest-pinned artifact/scope identity 改善了协作计划完整性，但不是 ABE revocation 方案 | `NOT_ADDRESSED` | 需要 grant/revoke/rotation 协议、旧密钥与缓存 Data 的处理、传播延迟、失败恢复和大规模 ACL 管理实验 |
| CTRL-1 | 979C/979D：Service Controller onboarding/policy update latency 和可扩展性未评估 | May-20 controller 是权限体系组件，但评价未单独计量 | Spec 163 没有消除 controller，也未补该实验 | `NOT_ADDRESSED` | 需要 controller 冷/热启动、并发 permission fetch、policy update、证书/key 分发、故障恢复和参与者规模曲线 |
| SCALE-1 | 979C/979D：provider/user/attribute/participant 规模过小 | May-20 实验入口覆盖最多约 10 providers/小型 topology，不能外推到大规模 | Spec 163 的 MiniNDN matrix 验证了 59/59 rows 和 23/23 gates，但它是状态/故障覆盖，不是规模曲线 | `PARTIALLY_ADDRESSED` | 需要扩大 users/providers/roles/DAG width/attributes，报告 control-plane bytes、收敛时间、吞吐、P95/P99 和资源占用 |
| LOSS-1 | 979A/979D：packet-loss recovery 的机制来源、分段大 Response 恢复不清 | May-20 有 loss topology、timeout/retry 相关路径，但没有足以支持“完整恢复”的清晰协议归因；二者都基于 NDN，不能把底层能力单独算给 NDNSF | Spec 163 对 plan/preparation/execution 失败提供 deadline-bounded compensation，并用 MiniNDN carrier 与 bounded histories 检查重排/失败边界 | `PARTIALLY_ADDRESSED` | 仍需区分 NDN/NFD retransmission、SVS、应用 timeout、NDNSF retry 的责任；增加 segmented large response、不同 loss/burst/reordering 下的恢复率和额外时延 |
| THREAT-1 | 979D：PIT flooding、恶意客户端绕过本地 outstanding-window | May-20 token/replay 检查不能防止邻居 PIT 被大量 Interests 占满；本地窗口也不能约束修改过的客户端 | Spec 163 的 token、digest、signature、lease 和 attempt fencing 加强执行完整性，但不是入口流量治理 | `NOT_ADDRESSED` | 需要 threat model、per-identity/network rate limiting、PIT occupancy、无效 service Interests、绕过 SDK 的攻击实验和降级策略 |
| CACHE-1 | 979D：NDN Content Store 对唯一 Request/ACK/Selection/Response 有何价值 | May-20 论文强调 NDN caching，但 per-request 消息唯一，未给出可复用对象和命中测量 | Spec 163 将 model shards、scope keys、DAG artifacts/results 命名并支持 retention/reuse，使“可复用 Data”更具体 | `PARTIALLY_ADDRESSED` | 需要证明缓存位置、Freshness/版本、授权失效和命中路径；测量 cold/warm cache、跨 provider reuse、字节节省和错误旧版本拒绝 |
| USE-1 | 979D：缺少真正需要多个 provider 的具体、结果语义明确的用例 | May-20 的多 drone 示例没有充分说明不同 provider 结果是否可互换 | Spec 163 以分布式推理 DAG、角色约束、模型切分、依赖和 exact provider assignment 给出非可互换的协作用例 | `PARTIALLY_ADDRESSED` | 需要真实多节点分布式 Qwen 端到端执行，证明每个 provider 的 role、输入/输出关联和最终语义正确；当前本地 CPU Qwen 与 MiniNDN byte artifacts 不可合并成 distributed-Qwen claim |
| OVR-1 | 979E：额外 signaling/computation overhead，尤其无人机等受限设备，未量化 | May-20 四阶段协议和安全处理必然产生额外消息/计算，但未做分项核算 | Spec 163 保存 row-specific evidence、raw timing 和 preparation byte attribution，便于后续计量 | `PARTIALLY_ADDRESSED` | 仍需 Request/ACK/ACK_CLOSED/Selection/preparation/recovery 的消息数与 bytes 分解、签名/加密/规划 CPU 时间、能耗和受限设备实验 |
| WRITE-1 | 979C/979D：术语、图文不一致、controller 放置、KP-ABE 引用等表达问题 | 属于被评审稿本身 | Spec 163 文档不能修改已提交稿件 | `NOT_ADDRESSED` | 新稿必须重绘流程图，区分 controller/control plane 与 data plane，修正 Drone/YOLO 示例，定义 KP-ABE，并明确 infrastructure-based/free 的含义 |

矩阵中没有 `FULLY_ADDRESSED` 项。原因不是 Spec 163 没有研究价值，而是评审问题大多要求**新实验、理论/对照或论文解释**；当前 Spec 163 主要完成了机制与有限环境下的正确性闭环。

## 5. Spec 163 当前证据锚点

Spec 163 当前 audit verdict 为 `PASS WITH EXPLICIT NO-CUDA DEFERRAL`，13/13 tasks 已完成。已保存：

- `results/spec163-local-docker-20260729_015529`
- `results/spec163-minindn-matrix-v2-20260729_022847`
- 59/59 MiniNDN matrix rows；
- 23/23 gates；
- 63 个 row-specific evidence references；
- 4 个 runtime assertions；
- 36 个 exhaustive bounded histories；
- 5 次 Qwen3 warmups 和 25 次完整生成。

但 [audit.md](../audit.md) 明确限制：

- 不声称 distributed Qwen execution、TigerCluster/large-model performance；
- 不声称 malicious-computation correctness、distributed atomicity；
- 不声称 deadlock/starvation freedom 或 universal optimizer quality；
- 本地没有 CUDA，GPU reload/reuse 指标为 `DEFERRED_NO_LOCAL_CUDA`；
- local CPU Qwen 与 MiniNDN byte-sized DI artifacts 不能合并成 distributed-Qwen performance claim；
- bounded history 不是任意调度的形式化证明。

当前 Spec 163 工作树并未形成一个可代表全部实现的 Git commit/tree。`HEAD` 是：

- commit `f179f779f2e6863f23d8387d38f6b45bb58a59ef`
- tree `d6d3534d87c511070bd2ae49c0f25ee674087917`

但 Spec 163 文档为 untracked，相关 DI/runtime 文件大量 modified/untracked，因此**不得把该 HEAD 称为 Spec 163 实现快照**。本次审计使用以下文件哈希作为临时锚点：

| 文件 | SHA-256 |
|---|---|
| `specs/163-di-collaboration-planning/spec.md` | `9812e6bf688c2512106ddd383546e205b7399bd1e40b03608b77439489baecf9` |
| `specs/163-di-collaboration-planning/plan.md` | `7c9419fdcfc067e34940edb6e35800a3d1d8ba99dee065bc7dc32c8e3de5a6b3` |
| `specs/163-di-collaboration-planning/tasks.md` | `1ae938c63c92237b5f1df707cc014c86e7c4ac4743e1c22581c8d5cbb838f2d1` |
| `specs/163-di-collaboration-planning/audit.md` | `0130a727b5e486f903ff789d60c9db7f4a78010965870b0e569895e3cd6dabbb` |
| `pythonWrapper/ndnsf/service.py` | `d8a2689135e1d71c9513f8ca940bccbd3cf0a269932e4cbd05300f6b8f5b1d25` |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/planner/presplit_first.py` | `de5ffde00f4477a17ed88c6dea797ac19904fb5b0f1197db593a147d7d64860c` |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py` | `f1186f957a66ecbe3a0019e43eb10cb640e4bf3ca0fed650938b5e606e95ecbc` |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py` | `46b5334570c10f02ececa1cf05e33802f1680e37b4250c2b3233bbbca3e314a0` |
| `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` | `108f20d9530a4cacc4d33c2d8500a4a331f3e5159dba110195cc37e325b441a5` |

## 6. 对下一篇论文的结论

Spec 163 **可以明显增强创新性叙事**：它把 May-20 的“单请求发现多个可选 provider”推进为“冻结候选集后，由可替换策略生成并提交多角色协作计划，再按 DAG/lease/attempt 约束执行和恢复”。这是机制层面的实质增量。

但是它尚不能完整消除 ICNP #979 的拒稿原因。最准确的论文结构是：

1. 将 `v0.1` 定义为 May-20 baseline，不把 Spec 163 能力回填到原稿；
2. 把 Spec 163 作为新的协作规划与执行贡献；
3. 用 ablation 证明新边界分别贡献什么；
4. 用公平 gRPC failover 和相关 edge/collaborative-inference 系统重做 baseline；
5. 单独补齐 NAC-ABE/controller/scale/threat/overhead 证据；
6. 在获得明确授权后，用 Spec 162 的真实 3×RTX 5000 Qwen 路径补 distributed-Qwen/GPU 证据。

当前最值得做的下一步不是改写创新性段落，而是先生成一个**可提交、干净、不可变的 Spec 163 release commit/tree 与多仓库依赖 manifest**，随后按 `REL-1 + BASE-1 + USE-1` 设计三组最高优先级实验。没有这个新快照，后续论文仍可能再次混淆“代码现在有”与“某次实验实际测过”。

## 7. 快照复核命令

```bash
git show-ref --tags v0.1
git cat-file -p v0.1
git show -s --format='commit=%H%ntree=%T%nparent=%P%nauthor=%aI%ncommitter=%cI%nsubject=%s' v0.1^{}
git show -s --format='commit=%H%ntree=%T%ncommitter=%cI%nsubject=%s' d42e6f8f66d4b38310647725aeb7544086f99b2d
git merge-base --is-ancestor v0.1^{} d42e6f8f66d4b38310647725aeb7544086f99b2d
git grep -I -E 'begin_collaboration|ACK_CLOSED|commit_plan|PreSplitFirstStrategy|ExecutionLease|GenericSelection' v0.1 -- .
git ls-tree -r --name-only v0.1 | rg 'NDNSF-DistributedInference|pythonWrapper'
```
