# Feature Specification: DistributedRepo Contract and Implementation Convergence

**Feature Branch**: `Repo`
**Created**: 2026-10-08
**Status**: Implementation authorized; qualification incomplete
**Updated**: 2026-10-08
**Input**: 用户要求静态审计当前 Repo 与理想 Repo 的差距，纳入刚发现的缺陷，停止 Spec192/193，建立 Spec202 写清需求、设计、修复与验收。

## Scope Boundary

本 Spec 是新的唯一推进入口。Spec192/193 停止新增实现和实验，保留源码、原始证据、失败与历史 checkbox；停止不代表它们完成或被回滚。目标取自两者已接受的产品意图与当前架构，不添加自动 placement、erasure coding、DI 领域状态或新 Python 状态机。详见 [gap-audit.md](gap-audit.md) 和 [supersession.md](supersession.md)。

用户已授权在Repo完成全部任务；交付物是满足本Spec的实现、真实回归与完整资格，不是文档集。当前执行状态只在[tasks.md](tasks.md)维护；原始baseline与历史审核记录保留，HEAD单独不能代表未提交overlay。

## Implementation and Documentation Budget

- 交付按行为修复、真实回归和验收计量；文档数量不计成果，不为改善比例增加无需求代码。已有契约足够时直接实现，禁止另写同义方案。
- 唯一职责：spec写要求，plan写owner/依赖/验证顺序，contracts写行为与候选规则，matrix/API清单写覆盖，tasks写当前状态。相同事实只维护一处，其他位置链接；旧batch/review/handoff仅历史参考，不续写、不恢复为执行入口。
- 普通修复只更新tasks对应行，内容限当前候选、结果、证据链接、阻塞和下一步，每行不超过400字符；命令、完整哈希、计数与失败经过放原始JSON/log。非平凡失败在既有failure-log记录一次。
- 不新增Markdown文件。核心文件上限为spec 200行、plan 60行、tasks 120行；修改其他现有文档原则上净增不超过0行。接口、重要状态、ownership、安全或迁移出现独有要求时才允许必要增量，并在对应任务行说明理由，先去重再扩写。
- 每次checkpoint检查文档新增文件数、净增行数和重复状态；不满足预算先压缩该次文档改动，不因此重跑未受影响的build/test。审查结论保存在既有evidence JSON/log，恢复工作只读tasks与其链接证据。
- 本约束优先于产生重复文档的流程模板；完整59case、每轮68场景、两轮136次、六profile及必要动态门不减少。保留原始失败与历史证据，没有实际证据不得DONE。

## User Scenarios & Testing

### User Story 1 - Own Durable Lifecycle and Truthful Publication (Priority: P1)

应用明确选择应用持有的 In-App 库或独立 Persistent 服务。前者只提交本地对象；显式 archive 后才创建选定远端副本。后者由用户在 readiness ACK 后选择目标与 ONE/QUORUM/ALL，一切副本事实来自 durable receipts。seed 前台屏障完成后 native owner 可继续复制，重启恢复不改变已发请求的事实。

**Independent Test**: 同时使用独立 In-App/Persistent 实例，核对归档前零远端写、归档后 exact selected commits、重启状态及真实未选节点零写；故障顺序覆盖先发 B 并超时，再在 C materialization 时发现源丢失。

**Acceptance Scenarios**:

1. 两种 mode 的 owner、root、lease、receipt 与作用域明确，内容身份相同不合并两种 mode 的副本数。
2. ACK 只证明候选能力；返回成功只计完整、认证且耐久的提交。
3. 幂等重放、冲突、source/target/owner 故障和重启保留 operation identity；已尝试但不知结果的目标不能变成未尝试。

### User Story 2 - Retrieve Verified Bytes Without Destroying Prior Output (Priority: P1)

消费者使用完整 handle/reference 读取，在全局 deadline 内按认证 Provider 去重、检查当前 LOCATION 并切换副本。只有完整校验且目的文件提交成功才交付。目录同步或最终 deadline 失败时，旧目的文件或原有不存在状态应恢复。

**Independent Test**: 真实 native API 中注入 post-rename directory-fsync EIO 与跨 deadline 延迟，覆盖旧文件/不存在两类目的路径；再运行三身份 failover/cooldown 与独立消费者字节校验。

**Acceptance Scenarios**:

1. 正常 fetch 完整验证 identity/digest/size/location/receipt，late Data 不进入成功结果。
2. 普通提交故障返回前恢复旧 namespace/bytes；验证失败不会交付部分内容。
3. 回滚本身失败明确报告 destination recovery failure，保留可定位的旧内容恢复材料并停止自动副本重试，不声称恢复成功。
4. 同一 logical fetch 每个认证 principal 最多一次；cooldown 不修改历史 receipt。

### User Story 3 - Separate History, Availability and Routing Security (Priority: P1)

历史 receipt、当前节点可达性、对象存在性和完整性分开。artifact-v2 与传统对象使用各自 owner；传统原始签名 Data 通过已验证当前 Provider hint 路由，保留原名和原始 wire。

**Independent Test**: artifact 在线缺失/截短/损坏、传统真实缺段、节点离线分别观察；O17 完整子场景验证 receipt/location、hint、原始 wire、缓存、失效副本与无候选拒绝。

**Acceptance Scenarios**:

1. `UNREACHABLE/UNKNOWN` 不冒充 `ABSENT`；历史提交不冒充当前持有对象。
2. 不要求 artifact `checkAvailability` 接受传统 segmented parent；真实 segment loss 由传统对象入口和存储 oracle 证明。
3. `DATA_RECEIPT/DATA_LOCATION` 元数据查询按 Core 授权；原名 Interest 不携带 NDNSF requester grant，hint 不等于授权或密码学来源证明。敏感原始 Data 由 producer 发布前加密，消费者凭密钥解密。

### User Story 4 - Preserve Object APIs and Recover Explicit Legacy Import (Priority: P1)

普通对象、range、segmented、Data packet、catalog、cache 与同进程 registry 保持公开边界；Persistent artifact profile 的传统写拒绝不得被 fixture 绕过。旧数据通过显式 native offline import 迁移，源 DB/WAL/SHM/payload 不变，旧复制/策略声明不变成新提交。

**Independent Test**: 公开入口逐方法映射真实调用；import 在 no-replace rename 后 parent fsync 失败，重放必须重新建立耐久屏障并核验目标/源 identity，不能仅因 COMMITTED manifest 存在返回成功。

**Acceptance Scenarios**:

1. 本地调用不被描述成网络；网络请求使用正确服务 profile、真实注册和授权 handler。
2. 缺段、越界、abort、delete、异常签名或 schema 不产生半提交成功。
3. 迁移阶段的 `INCOMPLETE` 与已暴露但耐久性未确认的 destination 分开；不删除完整目标冒充回滚。

### User Story 5 - Reuse One Native Repository Through Python and DI (Priority: P2)

Python 为 C++ 薄绑定，保留借用 owner 的寿命并释放 GIL 执行阻塞调用。DI 显式校验 generic identity 到 Repo handle 的映射；冷/暖缓存均遵守授权。Repo 不解释模型、张量、KV 或请求状态。

**Independent Test**: C++/Python 双向真实跨进程读写、实际 DI Provider 模型/输入/输出引用使用、冷/暖拒绝与零临时 Repo 写；退休 API 稳定拒绝且无副作用。

**Acceptance Scenarios**:

1. identity/handle/receipt/异常 attempt ledger 保持 native 事实；不存在第二份 Python 耐久 catalog。
2. 授权前不得落地材料；缓存或 prepared model 不增加 Repo receipt。
3. activation/KV/session state 无显式 snapshot/retention/access policy 时不自动发布。

### User Story 6 - Reproduce and Qualify the Complete Candidate (Priority: P1)

接手者能从明确 source/overlay、构建产物和有效配置复现全部公开入口。历史局部 PASS 可指导回归选择，但不能拼成当前候选完整资格。

**Independent Test**: 先完整 code-aware convergence，再受影响 C++ unit/生产 integration/Python conformance 与风险匹配动态门；最后对同一冻结 source family 和声明的 profile composition 做两轮完整未跟踪、五主机以上 MiniNDN 矩阵。

**Acceptance Scenarios**:

1. 继承 A22/O17/L4/N10/P6 共59稳定 case，保留子场景和新提交故障回归，不通过删 case 达成完成。
2. 三副本使用六独立主机，三选一未选 sentinel 使用七主机；每节点抓包、有限 child、实际 loaded libs 与 cleanup 都符合 manifest。
3. 签名 Sync REQUEST 映射可证明离线发布序列；Provider 顺序只按 native 顺序/源码推断，不伪称 wire 含未编码字段。

## Acceptance Evidence Contract

| Plane | Independent oracle | Cannot substitute |
| --- | --- | --- |
| native durable state | SQLite/manifest/payload/receipt、真实生产 owner 和重启 | ACK、marker、Python 模拟状态 |
| destination commit | 旧 inode/bytes 或 absence、post-rename hook 实际命中、恢复文件与故障分类 | 只匹配错误文本、仅 precommit corrupt case |
| network/auth | 内外签名、当前 Provider/location、五/六/七主机抓包与实际 handler | hint 推断密码学来源、fixture bypass |
| candidate | source/artifact/config/harness 哈希、可重建依赖、逐 case 双轮 | 混候选局部 PASS、HEAD 单独复现承诺 |

### Edge Cases

- rename 成功后 fsync/open-directory/最终 deadline 失败；回滚失败；已接受 commit 后 cleanup 失败。
- B 已发 STORE 后超时，C 尚未发请求时源损坏；迟到 B 响应不得改写终态 aggregate。
- legacy destination 已可见但 parent fsync 失败；冲突 destination、staging 双存在、source WAL 改变。
- stale/cross-reference LOCATION、旧 handle、新位置版本、重复 principal aliases、全部离线、错误 signer。
- artifact 截短与真实传统缺段的不同 owner；原始 Data 密文与元数据授权的不同边界。
- native owner/thread teardown、Python strong owner、异常结果复制、DI cache authorization。

## Requirements

### Functional Requirements

- **FR-001**: MUST 为当前 source overlay、架构 owner 和全部公开入口建立可追溯基线，分别标记 existing/planned/unverified。
- **FR-002**: MUST 保持 In-App/Persistent 生命周期隔离和显式 archive；用户决定目标及一致性，ACK 不证明提交。
- **FR-003**: MUST 由 native durable receipts 决定 publish 成败、seed barrier、异步状态和重启恢复。
- **FR-004**: MUST 保留 target attemptCount/未知结果；已发请求不得被报告为 NOT_ATTEMPTED/SKIPPED。
- **FR-005**: MUST 在 fetch 最终目录同步后检查 deadline；普通失败恢复旧目的 namespace/bytes 或 absence。
- **FR-006**: MUST 对不可恢复 rollback 给出显式 fatal recovery 结果、保留材料、停止 Provider retry；已接受 commit 的 cleanup 不得反转为 failed fetch。
- **FR-007**: MUST 校验完整 reference/handle/receipt/LOCATION 元组，Provider 去重、版本键控 cooldown 与全局 deadline。
- **FR-008**: MUST 分离 receipt history、reachability、presence、integrity、时间与观测来源；分别验证 artifact 与传统缺段。
- **FR-009**: MUST 保留原始 Data 名称、签名、wire；先查认证 receipt/location，再使用非空 Provider-specific hint，无有效候选则拒绝。
- **FR-010**: MUST 保持 Core 元数据授权与 producer 内容加密边界，不把原名 Interest/hint 当作 NDNSF grant。
- **FR-011**: MUST 逐方法覆盖传统对象/range/segmented/catalog/cache/local registry，明确真实网络与同进程边界。
- **FR-012**: MUST 采用 native 显式 offline import，保持源不变和 no-replace；已验证 destination 的重放重新执行 parent durability barrier。
- **FR-013**: MUST 保持 Python thin binding/native owner、跨语言语义、DI identity adaptation 与缓存授权。
- **FR-014**: MUST 稳定拒绝退休 API，不产生成功响应、重新选副本或存储副作用。
- **FR-015**: MUST 继承全部59 case及子场景、跨 MiniNDN persistence，新增每项实际缺陷的 closing native regression。
- **FR-016**: MUST 在正式验证前关闭全部 controlling gap，通过静态五 lane/convergence；静态、构建、行为、动态、资格分别记录。
- **FR-017**: MUST 冻结 source/runtime/harness/config/validation 与 profile composition，定义变更失效矩阵、无外部副作用的 closure mutation checks。
- **FR-018**: MUST 完成当前候选全部受影响测试及两轮完整未跟踪多节点矩阵，任何 OPEN case/异常 cleanup 禁止整体完成声明。
- **FR-019**: MUST 在计划中映射 Spec192全部目标、Spec193全部未完成任务与已发现缺陷；旧证据保持历史，不反填当前资格。

### Key Entities

`ArtifactReference/Handle`、mode-scoped receipt、operation/target ledger、current location/availability、destination commit/recovery、original signed Data receipt/location、legacy import manifest、candidate family/profile/run。字段与状态唯一详细定义见 [data-model.md](data-model.md) 和 [contracts](contracts/architecture-and-api.md)。

## Success Criteria

### Measurable Outcomes

- **SC-001**: 每个公开入口和每个继承目标都有 owner、requirement、task、真实 fixture/oracle、结果与证据；零未解释遗漏。
- **SC-002**: 两个 HIGH 和 legacy durability MEDIUM 各有真实 native red/repair/closing regression；普通 fetch 提交失败覆盖旧文件与不存在目的路径。
- **SC-003**: 已尝试目标不被抹除；历史 aggregate/receipt 与独立存储在迟到响应、重启后保持一致。
- **SC-004**: 授权、identity、hint、原始 wire、Python/DI 和 migration 边界在正确 owner 中闭合，不新增无需求 API/状态机。
- **SC-005**: source/build/profile/closure 可复现，静态和全部受影响门通过；未观测项不计 PASS。
- **SC-006**: 全59 network case及网络子场景在同一冻结 source family 下两轮完整资格 PASS，各节点抓包、有限 child 和 bounded cleanup 一致；R01–R07另有最终candidate的native/适用dynamic closing证据，不要求移植为network fault。

## Assumptions

- “理想 Repo”采用 Spec192/193 接受的可复用、native-owned 产品目标；不推断额外部署或性能目标。
- 不承诺任意 OS/磁盘永久故障下能恢复 namespace；显式 recovery failure 保留材料，不声称 power-loss 已验证。
- 本机 Ubuntu20.04、system Boost1.71、固定 Experimental 依赖；不得为了检查门构造 Boost1.74 prefix。
- 当前授权覆盖实现与本机功能资格；GSD复用tasks registry，不另建状态机。ARS统计/文献门不适用；Tiger/SIF、模型benchmark与push不在范围内。
