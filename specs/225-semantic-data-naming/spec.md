# Feature Specification: Semantic NDNSF-DI Data Naming

**Created**: 2026-11-13
**Status**: IN_PROGRESS
**Source baseline**: `spec253@cd9246fdc18007b32e45f0c742a9ed98f33f8976`
**Progress Timestamp**: 2026-11-20 14:57 -06:00
**Input**: 把已审查的 NDNSF-DI 命名设计整理为 Spec 225，说明完整细节并审计修正；暂不切换工作。

## Outcome and Scope

使 NDNSF-DI 的实际 Data 名称直接表达 Producer、对象种类和模型/执行坐标，保留一个
必要的表示身份承诺，继续支持精确预取、验证、缓存、Repo 原包复用和 KV 移交。

初始交付限于设计、计划和任务。用户已明确激活 Spec 225 实现，当前进度和验收证据
见 [tasks.md](tasks.md)。协议行为和已安装推理的验收以各任务的独立运行证据为准。

名称目录和编码只在 [contracts/naming.md](contracts/naming.md) 定义；生产接线、字段和
失败恢复只在 [contracts/integration.md](contracts/integration.md) 定义，避免多份表互相漂移。

## User Scenarios & Testing

### User Story 1 - Inspect Meaningful Data Names (Priority: P1)

研究者从名称分辨模型与量化标签、材料 bundle 编号、独立发布的模型碎片、PP 输出、
TP partial、反馈和 KV 状态。模型碎片能够看出切分方式、层/图范围、rank 与入口。
完整身份和签名验证仍然有效。

**Independent Test**: 以生产 builder/parser 生成并接纳目录中的实际对象，独立 TLV 向量
核对字段、组件类型和实际 Producer；API-only/embedded/local 项不新增网络发布。

**Acceptance Scenarios**:

1. **Given** 已验证的 Qwen3-0.6B INT4 元数据，**When** 发布材料 bundle 12，
   **Then** 名称显示模型坐标和 `bundle/12`，精确区分该表示而不堆叠所有摘要。
2. **Given** 两 Provider 的相同 rank 数值，**When** 分别发布 partial，
   **Then** 由各自完整 Producer 前缀区分，错误 step/rank/Producer 被拒绝。
3. **Given** 同一 Transformer 源图的 PP、TP、hybrid 配方，或适配器验证过的图分区配方，**When** 按 [N4](contracts/naming.md#partition-coordinates-for-derived-artifacts) 构造派生碎片的发布引用，
   **Then** 名字区分实际切分方式、范围和入口。相同 degree/rank 的均分与加权切片使用不同的实际布局标签，验证时仍核对完整配方和内容身份。
4. **Given** Provider 按同一份 canonical 材料清单本地组装不同角色的碎片，**When** 选择另一种切分或部署位置，
   **Then** 用角色/配方识别本地文件，原有材料和 Repo 原包继续按原名读取，无需额外发布本地碎片。

### User Story 2 - Prefetch Without Weakening Admission (Priority: P1)

Provider 在 activation 产生前，从已接纳计划推导 exact names 并有界预取；收到实际
manifest、签名和受保护内容后才让输入进入 runner。

**Independent Test**: 生产密封/依赖链上的 native fixture 在输出前记录预取 Name，之后
对照真正发布的原包；故意修改 binding、layout、保护或范围，检查拒绝与资源释放。

**Acceptance Scenarios**:

1. **Given** 固定的发布/保护快照，**When** 在已接纳硬界内调整本地 cwnd/timeout/Repo locator，
   **Then** 已有对象名字不变，获取预算和授权仍受验证。
2. **Given** 可变大小输出，**When** 实际分段数与合法 estimate 不同，
   **Then** 以已验证 manifest 为准调整窗口，超出已接纳硬边界则失败。
3. **Given** EOS/stop早于maxTokens，**When** 下一轮转为checkpoint finalize，
   **Then** 仍使用事先可推导的continuation Name，实际转换由认证lineage接纳。
4. **Given** inline-first 模式，**When** manifest 包携带 segment 0，
   **Then** 不额外等待不存在的 segment 0 Data，framing 不得在发布后同名切换。

### User Story 3 - Reuse Models and Checkpoints (Priority: P1)

准备方只发布一次模型，之后离线。当前获授权 User 继续引用原包并为 Provider 授予
当前使用权限；换 Repo 或 Provider 不重发模型，也不把原包改到消费者空间。

**Independent Test**: 准备、存储、退出写入方，重新打开持久 store，用真实生产读 API
读取原 Name/wire，再执行授权读取或恢复；新 User 和 warm 请求仍验证原对象与当前权限。

**Acceptance Scenarios**:

1. **Given** 已保存的 StoredModel handle，**When** 准备方离线并启动新请求，
   **Then** 使用原 root/index/object names，当前 capsule 独立更新。
2. **Given** 旧 Provider 的受保护 checkpoint，**When** 新 Provider 恢复，
   **Then** 验证原 Producer、状态和当前授权；新输出才属于新 Provider。
3. **Given** 仍需复用的既有模型包，**When** 新命名实现安装，
   **Then** 既有 exact 引用继续可读，Repo 不改名、不重新签名，不强制批量重发。

### Edge Cases

- 完整 URI、UTF-8、含 `/` 的字段、超长 label/Name、错误组件类型和非 canonical 数值。
- 已密封计划引用自己派生的 `<RepresentationId>`，或 consumer-specific slot 被错误混入共享 output 身份。
- 保护/证书/分段变化、崩溃后原包丢失、迟到输出、重复发布、冲突终态和取消中的预取。
- 未知量化/size 元数据不能从模型名猜；INT4 权重不意味 activation 也是 INT4。
- 保留模型 revision、NDN typed Version 和外部库协议版本；不增加发布版本路径或协商。

## Requirements

### Functional Requirements

- **FR-001 — Producer Ownership**: System MUST 在真实 Producer 的完整前缀下命名应用
  Data，验证 namespace 发布权；Repo 副本保持原始 Name/wire/signature。
- **FR-002 — Complete Semantic Catalog**: System MUST 按 N4–N6 区分所有实际对象族，
  保留已声明语义坐标。派生制品按 N4 的切分坐标和已验证布局区分；将独立制品、物理材料
  bundle 和本地组装文件分别归于各自链路。
- **FR-003 — Canonical Components**: System MUST 使用完整描述性占位符所对应的统一
  组件规则，无发布版本路径、静默截断或跨语言 wire 差异；标准 typed 组件保留。
- **FR-004 — Acyclic Immutable Identity**: System MUST 以无自引用的表示承诺区分
  保护/编码/发布，重发原包；不能用新名字改写终态或绕过重新接纳。
- **FR-005 — Exact Prefetch**: System MUST 在 runtime 输出前密封共享 reservation，
  将获取策略与对象身份分开；最终 manifest、framing、预算和执行依赖仍严格验证。
- **FR-006 — Portable Reuse**: System MUST 复用稳定模型、StoredModel 与所需既有 exact
  引用，当前 authorization/capsule 独立更新；单纯换 CPU/CUDA 或 Repo 不重准备模型。
- **FR-007 — Coordinated Integration**: System MUST 同步生产者、codec、引用、签名/
  授权、AAD/密钥派生、reader/prefetch 和缓存身份；保留现有 Core 和外部协议载体。
- **FR-008 — Lifecycle and Regression**: System MUST 保持取消、超时、原包恢复、资源
  上限和 PP/已有 TP 行为，复用 Spec252 共享 partial 与接收侧归约，保留能力不满足时的兼容路径。
- **FR-009 — Evidence and Activation**: System MUST 在激活后先做源码/契约一致性审查和
  native 行为验证，再做安装后 MiniNDN CPU cold/warm；本次文档不得伪记实现完成。

### Key Entities

- **Semantic Object**：Producer、类别、模型或执行坐标和完整经过验证的业务身份。
- **Publication Reservation**：一次不可变发布的身份/保护配置，由既有发布或规划 owner 持有。
- **Retrieval Reference**：exact Name、必要 digest pin、locator/hint 和获取预算；地址不改内容身份。
- **StoredModel Handle**：本地私有 root pin 与密钥材料，不是公开 Data。

## Success Criteria

- **SC-001 — Catalog Coverage**: N4–N6 每一行有实际 publisher/consumer 或非独立发布
  理由；不得用 whitelist、私有 cache key 或退役 Python 路径充当网络证据。
- **SC-002 — Wire Agreement**: 所有改动名称通过生产 native round-trip、独立 TLV
  golden vectors 及薄绑定一致性检查；完整 URI/数值/segment/sequence 边界无歧义。
  固定同一个 Qwen 源图及版本，比较 PP、TP、hybrid 及同坐标不同加权布局。用各自的
  YOLO/automatic ONNX fixture 验证图分区。各 Name 都须与认证配方一致，错
  cut/rank/profile/entry 被拒绝。
- **SC-003 — Security and Immutability**: 错误 Producer/recipient/request/attempt/step/
  rank/layout/epoch、同名不同 wire 和冲突终态在对应生产边界被拒绝。
- **SC-004 — Early Retrieval**: runtime 输出前可推导实际 exact names；普通与 inline-first
  framing 均能接纳，合法 estimate 差异不误拒绝，超限和取消有界释放。
- **SC-005 — Reuse Across Restart**: 模型包和 checkpoint 至少通过一次写入进程结束后
  的持久读回，原 wire 一致；准备方离线、当前授权更新和错误密钥/pin 负例成立。
- **SC-006 — Installed Inference**: 冻结候选的已安装两 Provider MiniNDN Qwen3-0.6B
  CPU 32-token cold/warm 通过独立输出/KV oracle，记录名称、请求耗时、ONNX 时间、
  传输字节和清理。TP 只对激活时已有支持范围验证，不能把未完成 TP 功能塞入命名任务。

## Assumptions and Exclusions

不新建 naming service、别名翻译层、通用版本兼容框架、第二套 publisher/调度器、外部库
改动、GPU/Tiger campaign 或全模型重发布。现有 Core 发布冻结规则先检查生产行为和
原回归；只有证实命名接纳链存在缺口才在原 owner 修补，不先重构 Core。

激活 Spec225 时将当时的生产源码与本次固定提交重新对齐。实现计划、任务及本轮审查
分别见 [plan.md](plan.md)、[tasks.md](tasks.md)、
[audit.md](audit.md)。
