# Feature Specification: Placement Observation Collection and Planning

**Feature Branch**: `Experimental`<br>
**Created**: 2026-09-28<br>
**Status**: Draft
**Input**: User description: improve the collection and delivery of Provider and User-to-Provider information so the native placement strategy can make better assignments.

## User Scenarios & Testing

### User Story 1 - Trustworthy directed observations (Priority: P1)

用户希望策略拿到的是有方向、有来源、有时间边界的实际观测：User 到各候选 Provider 的服务路径信息、Provider 间真实依赖传输信息，以及当前请求相关的模型驻留/缓存状态。缺失值必须保持缺失，不能用 `0`、默认带宽或请求服务耗时伪装成纯网络 RTT。

**Why this priority**: 观测不可信时，策略优化会把错误数据当作事实，可能比当前稳定策略更差。

**Independent Test**: C++ production oracle 给定有效、缺失、过期、错方向、跨 boot epoch 和错误签名的观测，分别验证采纳/拒绝结果及其可追溯来源。

**Acceptance Scenarios**:

1. **Given** User 为同一 request/attempt 收到多个 Provider ACK，**When** native placement 输入形成，**Then** 每条 User-to-Provider ACK-path 样本保留 `ACK_RTT` 类型，且不被描述为纯链路 RTT。
2. **Given** Provider 已发生跨 Provider 的真实依赖 Data fetch，**When** 其签名 offer 报告该 directed edge，**Then** `srcPeer`、`dstPeer`、样本方法、有效时间、字节数与超时/Nack 计数可核对。
3. **Given** 一个值无样本、过期或身份不匹配，**When** snapshot 被验证，**Then** 该值为 unavailable 并且不会被策略当作零成本或高置信度测量。

### User Story 2 - Request-scoped cache and capacity view (Priority: P1)

用户希望在 ACK/Selection 前获知候选 Provider 对当前候选角色所需材料的驻留情况、可用资源和队列状态，以避免选到必须重复下载/组装的 Provider；该信息只能覆盖当前请求的允许内容，不得枚举或泄露任意本地缓存。

**Why this priority**: 角色驻留、设备资源和排队状态会直接改变准备成本及可执行性。

**Independent Test**: C++ offer/selection oracle 对照请求的 model/graph/artifact digests、Provider boot/process/topology identity 和 freshness，验证真实 hit、explicit miss、unknown 与伪造 cache row 的区别。

**Acceptance Scenarios**:

1. **Given** Provider 具有与当前 role 完全匹配且仍有效的 residency row，**When** V4 offer 进入策略，**Then** 仅该已验证内容可影响 reuse/preparation cost。
2. **Given** Provider 没有匹配缓存或缓存状态未知，**When** offer 被构造，**Then** 明确区分 `known-empty` 和 `unknown`，不把 `hasModel` 当成 exact reuse。
3. **Given** User 尚未完成授权/Selection，**When** 采集 placement observations，**Then** 不触发未选中 Provider 的模型材料预取或组装。

### User Story 3 - Global role placement uses observed transfer cost (Priority: P1)

用户希望策略结合角色依赖图、可核对的传输规模、Provider 间 directed edge、User ingress/egress、缓存/队列和硬件可行性选择整体方案，而不是只逐个角色贪心排序。遇到缺边、过期数据或搜索预算不足时，应确定性回退到当前策略并记录原因。

**Why this priority**: 单个 Provider 的 RTT/带宽排序不能代表跨 Provider 角色组合的成本；角色间数据依赖会改变实际完成时间。

**Independent Test**: named native C++ production target 使用独立手算 oracle 比较完整小型角色图，验证全局候选排序、硬约束保持、相同输入确定性及缺测回退。

**Acceptance Scenarios**:

1. **Given** 两个都满足身份、授权、角色、设备和内存硬约束的候选布局，且其中一个具有更低的新鲜 critical-path 传输/准备估计，**When** observation-aware strategy 运行，**Then** 选择较低成本布局并绑定所用 snapshot digest。
2. **Given** 所需 cross-provider edge 的传输大小或观测缺失，**When** 策略评估，**Then** 不插入 synthetic 默认值，调用既有 deterministic strategy 并在 proposal/trace 中记录 `OBSERVATION_INCOMPLETE`。
3. **Given** 评估达到既有 policy/deadline budget，**When** 没有完整候选，**Then** 通过现有 fallback 完成或返回既有无可行候选错误，不越过取消/截止时间。

## Requirements

### Functional Requirements

- **FR-001**: Core MUST 按测量类型保留实际生产样本；`ACK_RTT`、大对象/依赖 `DATA_FETCH` 和任何未来 `ACTIVE_PROBE` 不得互相冒充。
- **FR-002**: 每条用于策略的网络记录 MUST 明确方向、观测者、端点身份、观测时间、有效期、样本数和测量范围；不可证明身份/新鲜度的记录 MUST 被视为 unavailable。
- **FR-003**: V4 Provider offer MUST 提供当前请求候选范围内的 Provider-local resources、queue/capacity 和 residency 状态，并区分 observed、known-empty 与 unknown。
- **FR-004**: V4 offer MUST 以 Provider 身份签名并绑定 request ID、attempt、service、model/graph/candidate digests、Provider boot epoch 和期限；Core ACK authentication 与 DI offer signature 都必须验证。
- **FR-005**: User MUST 将本地 User-to-Provider 观测与通过验证的 Provider-to-Provider/Provider residency 观测合成不可变 `PlacementObservationSnapshot`，将其 digest 绑定 proposal/sealed selection，禁止跨 request/attempt 或跨 boot 复用。
- **FR-006**: Native placement MUST 先执行当前 admission、role/backend/device/memory、授权、placement 和 cancellation 硬约束，再比较估计成本；测量不得扩大可执行权限。
- **FR-007**: Observation-aware placement MUST 根据实际 role dependency DAG 计算有方向的跨 Provider 传输成本，并把 User 输入/最终输出边只在确有数据传输且大小可核对时计入。未知 `estimatedBytes` 不等同于零字节。
- **FR-008**: Selection MUST 以可复现的 tie-break 顺序处理同分候选，并将策略版本、snapshot digest、成本分解和 fallback reason 纳入可核对记录。
- **FR-009**: V3 canonical wire/signature behavior MUST 保持兼容；仅当本 request 的全部候选 Provider 声明支持 V4 时才发 `DI_PLACEMENT_V4` request，单个 request/attempt 不得混合 V3/V4 offer。否则继续走现有 V3/default path；V4 观测无效时，在已验证的 V4 offer 值上沿用当前 baseline 排序语义并记录原因，不把 V4 伪装成 V3。
- **FR-010**: Observation collection MUST 有样本/字节/时间上限，不得为 placement 在 ACK/Selection 前下载、预取或组装整模型；V1 默认复用真实生产流量观测，不新增周期性 O(n²) active probes。
- **FR-011**: Placement snapshot MUST 排除原始 Data name、请求输入/KV、grant/key、明文材料及不属于当前候选的缓存清单；签名和摘要只保护允许披露的 metadata。
- **FR-012**: Formal validation MUST 走具名 C++ production target/oracle；MiniNDN 对照必须使用已安装软件并观测真实 Prepare→ACK→Selection→placement-bound fetch→Provider assembly/execute→terminal response→cleanup。

### Key Entities

- **NetworkObservation**: 一条带方向、测量类型、端点身份、真实样本及 TTL 的网络边观测。
- **PlacementProfileCapability**: 单个候选 Provider 对请求级 V3/V4 placement profile 的已认证、有时效声明。
- **SelectedPlacementProfile**: User 对冻结候选集为单个 request/attempt 选择的 profile 及 capability source digests。
- **ProviderResourceView**: 当前候选角色所需、由 Provider readiness/resource collector 实测的资源与 worker-capacity 摘要。
- **ProviderResidencyView**: 当前候选 digest/role/rank 在单个 Provider 的 known hit、known miss 或 unknown 状态。
- **PlacementObservationSnapshot**: 将当前 request/attempt、验证过的 V4 offers、User-local path observations 和 graph byte estimates 绑定为一个不可变输入。
- **PlacementCostBreakdown**: 每个角色节点、依赖边、User ingress/egress、cache/preparation、queue 和策略回退原因的估算明细。

## Success Criteria

### Measurable Outcomes

- **SC-001**: V4 placement 中使用的每条观测都具有可验证的 source、方向、sample kind、endpoint identity 和有效期；没有合成 RTT/bandwidth/default 值进入成本函数。
- **SC-002**: 对 assignment state 数不超过冻结上限的 C++ oracle 图，V4 选择与独立穷举 oracle 的最低估算成本一致；搜索被裁剪时，重复相同输入仍给出相同结果，且明确记录这属于有界启发式。
- **SC-003**: 对 stale、tampered、wrong-provider、wrong-attempt、wrong-boot、wrong-topology 观测，named C++ production selectors 全部拒绝或转为 unavailable，且 authorization/Selection 不被旁路。
- **SC-004**: 在预先冻结的非对称两 Provider MiniNDN 场景中，完整真实推理链路的 p50 latency 相对当前策略至少降低 10%；对称控制场景的 p50 不得回退超过 5%。同一源码/模型/负载、多个配对轮次和实际部署组件身份必须绑定证据。
- **SC-005**: 策略决策耗时始终小于请求传入的 `NativeCandidateBudget.maxPolicyMs`，并记录 p50/p95、搜索展开数及 deadline/fallback 结果；该预算不被改大来遮蔽开销。
- **SC-006**: 当可用观察覆盖不足时，V4 明确报告 baseline fallback；真实资格记录区分 baseline 和 optimized candidate，不把单 Provider/静态 selector/ACK 成功当成性能 PASS。

## Assumptions and Reset Disposition

- `Experimental` 已按用户要求精确重置到 `origin/AutoInstall` 的提交 `ad53a4031e4d3f052c3c154fece656dbecdca2df`；分支上此前的本地活动 Spec 工作（包括 Spec193）记为 `STOPPED / NOT COMPLETE`，不继承、不补勾、不再恢复。此记录不把 AutoInstall 中原本已归档的历史任务重新解释为完成。
- Spec194 first uses production event telemetry already owned by Core. Missing edges remain missing; event-driven observations do not guarantee a cold-start matrix.
- V4 is a new, opt-in placement contract until its production qualification passes. V3 remains the compatibility path.
- Current source findings are recorded in [research.md](research.md) and [evidence/current-source-audit.md](evidence/current-source-audit.md). Exact target interface is still `DRAFT` pending the source/ownership gate in T001.

## Out of Scope

- Changing authorization/grant semantics, cache lease rules, model material formats, Repo publication semantics, tensor numerical behavior, or admission feasibility rules.
- Sending model bytes as telemetry, probing with ordinary inference requests, or claiming a pure network-layer RTT from application service latency.
- Switching the default policy without the complete C++ and real MiniNDN comparison evidence in SC-004.
