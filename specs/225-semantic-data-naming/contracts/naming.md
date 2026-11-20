# Naming Contract: NDNSF-DI Semantic Data Names

**Status**: DRAFT / NOT_ACTIVE — 拟议规则，尚未实施。
**Design binding**: [spec.md](../spec.md) FR-001–FR-009；生产接线见 [integration.md](integration.md)。
**Source baseline**: `spec253@cd9246fdc18007b32e45f0c742a9ed98f33f8976`。本目录给出
Spec225 的目标语法。现行实现和原包兼容边界见 [integration.md](integration.md) 与 [audit.md](../audit.md)。

本契约是名称目录与 wire 编码的唯一详细定义。正文使用完整占位符，不加入发布版本号；
现有模型 revision、typed Version、body schema 和外部库协议版本保持各自职责。

## N1. Identity and Ownership

从名称看出“谁生产、哪种对象、哪个模型或执行位置”，同时保持精确预取、不可变缓存、
授权校验和跨实验复用。所有实际发布的 Data 都在本目录中有归属；同一承载语法下的不同
业务消息也列出，但不为每个 JSON schema 新建一种网络对象。

使用三层身份：

1. **语义坐标**：模型、材料编号、层区间、step、rank 等；公开且对解释对象有用。
2. **不可变表示**：最后一个 `rep/<RepresentationId>` 区分实际发布实例、保护和编码。
3. **完整验证事实**：完整模型/计划/布局/授权身份、实际字节摘要放在经过验证的
   descriptor、manifest 或消息中，不把每个摘要都重复放入 Name。

Repo 只保存原始包，不成为模型或 activation 的 Producer。Provider 换机器、换 CPU/CUDA、
换 Repo locator，不应使已有模型重新命名、导出或发布。转换、量化或切片实际改变字节时，
派生制品由真正的生成方命名。

## N2. Component Encoding

[NDN Name 规范](https://docs.named-data.net/NDN-packet-spec/current/name.html)定义分层组件，
并不要求全部可读。[命名约定](https://named-data.net/wp-content/uploads/2021/10/ndn-tr-22-3-ndn-memo-naming-conventions.pdf)
鼓励表达数据语义，并为 segment、version、sequence 规定类型。本设计遵循这些区别：

全文用 `<DescriptiveName>` 标示占位符，尖括号不是实际 Name 字节。完整前缀和
`<ModelPath>`、`<ServiceName>`、`<RequestName>` 等完整路径可能展开为多个组件；单个
标识、编号和摘要仍按下表编码。路径中的 `model`、`rep`、`DATA` 等固定文字保持原样。

| 记号/字段 | 唯一定义 |
| --- | --- |
| `<Producer>`、`<User>`、`<Controller>`、`<Authority>` | 实际 Producer、User、Controller、Authority 的**完整多组件前缀**；不是 rank 或 Repo 地址 |
| `<Provider>` | 特指完整的服务 Provider 前缀；与通用 `<Producer>` 区分，Producer 也可能是 User 或模型准备方 |
| 命名方案 | 项目尚未发布，不增加发布版本号组件；模型 revision、NDN typed version 和外部协议版本各保留原有含义 |
| 普通数值 | Generic 组件中的 canonical 十进制；`0` 或不带前导零的正数；层区间为半开区间 `[LayerBegin,LayerEnd)` |
| `seg=<SegmentNumber>` | 真正的 SegmentNameComponent；不是字符串 `"seg=0"` |
| `seq=<SequenceNumber>` | 真正的 SequenceNumNameComponent，仅用于有序事件/流，不用于 layer、rank |
| `v=<VersionNumber>` | 真正的 VersionNameComponent；仅保留有版本语义的现有协议/分段 API 用法 |
| `<RepresentationId>`、`<PackagePublicationId>` | 一个 32-byte Generic 组件；URI 中会转义。文中的 `<RepresentationId0>` 等仅为显示占位符 |
| `NameComponent(<Name>)` | 将完整 Name 的 canonical URI 可逆地编码为**一个** Generic 组件；包含 `/` 时转义，不截断、不作为子 Name 展开。这是文档编码记号，不是路径中的固定文字或新 API |
| `<RequestId>`、`<ConversationId>` | 公开逻辑request/conversation标识，单组件；完整身份与每attempt的Core RequestName在原binding中认证关联，不能从prompt/token或恢复后缀猜测 |
| `<Attempt>` | 同一逻辑请求的执行代次，不是Interest重传或token轮数；当前流式路径首次为1，满足既有恢复条件时仅允许一次replacement为2；warm新请求从1开始 |
| `<RoleId>`、`<StepId>`、`<PortId>` | 计划中声明的公开单组件标识；完整业务 Name 若存在则另存绑定，不截取末尾猜身份 |
| `<ModelPath>` | `<ModelFamily>/<ModelSize>/<Quantization>/revision/<RevisionLabel>`；例如 `qwen3/0.6b/int4/revision/r3` |
| `<ObjectPrefix>` | 下文某行定义的对象根；对象 payload 的 Data 是 `<ObjectPrefix>/DATA/seg=<SegmentNumber>`；保护/分段 manifest 是 `<ObjectPrefix>/MANIFEST` |

模型标签由已验证的准备元数据给出：`int4` 还需在元数据中绑定实际量化算法、group size、
算子和完整 source revision，不能从文件名猜。`r3` 只是公开标签，完整 source/content
identity 仍受验证。没有 size/quant 等事实时使用明确的 `unspecified`，不冒充已确认值。
新发布不得悄悄省略固定位置组件；解析器按 family schema 验证合法字段组合。

现有 `makeNamingLabelComponent()` 会截断到 64 个 UTF-8 字节；新语法**不使用截断标签作
身份**。公开 label 限制为 1–64 UTF-8 字节，超限明确拒绝；`NameComponent(<Name>)` 保存完整值。
保留现有 1024-byte Name wire 上限，超限报错，不自动截短或换 hash。完整字段和类型必须
进入 C++/binding golden wire vectors，不能只比较 URI 显示。
分段 payload 上限还须按实际 Name、签名、加密 envelope 和 FinalBlock 开销从
`MAX_NDN_PACKET_SIZE` 中扣除，不能把 8800 字节全部当 Content。先用固定 32-byte `<RepresentationId>`
占位计算 Name 长度/segment budget，再定 geometry 和 `<RepresentationId>`，最终检查签名包 wire 长度；
不通过反复改变 `<RepresentationId>` 来试探 segment size。

`<RepresentationId>` 不是标准的 ImplicitSha256DigestComponent。后者是整个 Data wire 的逻辑最后组件；
`params-sha256=` 是带 ApplicationParameters 的 Interest 所需参数摘要，见
[Interest 规范](https://docs.named-data.net/NDN-packet-spec/current/interest.html)。
二者都不因“减少 hash”而删除。FreshnessPeriod 也不替代版本或授权有效期。

## N3. Representation Identity

统一 `<RepresentationId> = SHA256(domain-separated canonical reservation descriptor)`。
沿用现有 canonical digest 编码机制，不引入第二套 JSON 排序规则。descriptor 至少绑定
Producer 的完整 Name、对象类型和完整语义身份、publication nonce、保护模式/
key epoch/recipient scope、编码与已确定的 segment geometry、签名/证书配置及影响 wire 的元数据。
不含私钥、对称密钥，也不含对象自己的最终 Name、最终 manifest hash 或最终签名值。

| 对象 | 额外绑定什么；何时可知 |
| --- | --- |
| 已知模型/材料 | 完整 model/content identity、对象坐标和已知内容摘要；发布前已知 |
| activation、partial、feedback | 逻辑request与当前Core RequestName、attempt、源侧程序、producer epoch、原operation index、step/output slot、rank、layout/security及预分配publication instance；**不依赖未来输出或停止判定** |
| 输入、Core 消息、流事件 | 当前调用/流身份、接收范围和本次不可变 publication instance；内容摘要由受保护消息/manifest 绑定 |
| KV checkpoint | 已导出的状态身份、payload/key-envelope/证书身份、布局和分段参数；不能只保留原来的两个 hash 之一 |
| StoredModel package | `<PackagePublicationId>` 是同一构造的 package publication commitment；先分配 publication instance，绑定模型与包保护配置，随后生成包内对象 |

这些是**发布身份承诺**，不是统一声称“`<RepresentationId>` 等于内容 hash”。模型内容可提前绑定；运行时内容
在产生后由已验证 manifest/消息绑定。不同 endpoint 对同一 producer output 的 consumer
在同一已接纳group/operation保护快照内，路由策略不改变该output的表示；不同capability、
operation key、recipient scope、layout或字节对应不同表示，允许同一逻辑output有多个R。
consumer 的 target step/slot 和 edge identity 在 sealed dependency 接纳中验证，不因为
另一个 consumer 接同一 output 就改变它的名字；recipient-specific protection 则例外。

运行时 reservation 的 owner 是现有 User/planner。它从 Provider ACK 接纳完整 Producer、
证书/保护 generation 后，在现有 assignment/endpoint 中分配并密封 publication seed；
每个 round/step/output 从同一 seed 和完整执行绑定确定性派生 `<RepresentationId>`，所有 consumer 和 Producer
接纳同一descriptor。Producer不得在计算后再随机改变影响名字的reservation输入。
普通protected publication沿用原随机nonce策略；NDNSF_DATA_V1 tensor沿用现有
`deriveNonce(capability, manifest, exactDataName, segmentNo)`派生，不新造KDF。
实际cipher nonce不反向进入预取名字。准备方/StoredModel自己持久化publication seed。
输入由 User 在现有 Selection binding 中冻结；事件沿用现有 StreamBinding 的已接纳
generation/epoch 分配，不另建 reservation 消息。

这里的 plan binding 是**生成派生名字之前、本输出的源侧执行事实**：源程序/step、artifact、
Producer assignment、output slot、布局/安全范围和 invocation generation；完整plan及
consumer target edge另外接纳，不能把outgoing consumer-specific事实藏在整体plan hash里。
先用现有 canonical
机制对这些源字段取承诺，再派生 `<RepresentationId>`/Name，最后密封包含它们的完整 projection。验证方从
同样源字段重算；最终 plan digest、endpoint digest、完整 Name/`<RepresentationId>` 自身和包含这些值的
manifest digest 不得反过来成为 `<RepresentationId>` 的输入。这是现有 serializer 的有向构造顺序，不新增
第二个计划 owner 或网络发布；否则“plan 包含 `<RepresentationId>`、`<RepresentationId>` 又 hash 整个 plan”会形成自引用。

模型、输入和 runtime tensor 的构造顺序为：确定语义/执行绑定及保护实例 → 得到 `<ObjectPrefix>` 和 segment names → 以这些
exact names 和上下文构造 AEAD AAD/派生密钥 → 加密、分段、签名 → 生成并签名最终 manifest。
根 manifest 的最终 wire digest 放在外部 reference 中，不能塞回自己的 Name/AAD。
**KV 保留已有 seal-first 顺序**：先以 checkpoint header metadata 作 AAD 导出/加密一次，
再从 encrypted wire 和完整 packet descriptor 计算 `<RepresentationId>`，最后命名并签名 `<CheckpointPrefix>`/segments。
最终 `<CheckpointPrefix>`/`<RepresentationId>` 不反向进入之前的 KV AEAD。恢复同时核对 header 的完整 checkpoint/Producer
身份、packet descriptor 与原始签名，不为统一外观而重写既有 KV 加密协议。

一次 reservation 只允许产生一份最终 manifest 和一组原始 Data wire。相同名字重发时
返回原包；崩溃恢复要复用持久化包/receipt。无法复用而必须重算、换 key、重签或重加密时，
分配新表示并发布经过认证的新引用。已密封 endpoint 不能原地换 `<RepresentationId>`：须经现有恢复/重规划
流程产生新 attempt/epoch 和接纳记录；纯模型重封装产生新 publication 引用。
已发送的旧引用不能悄悄指向新字节。

在同一已接纳发布/保护快照内，本地等待超时、cwnd、预取窗口/estimate、Repo 地址和
forwarding hint 是**获取策略**，不进入对象表示身份，也不重建已冻结的endpoint/manifest。
已密封的授权硬预算、lease、允许consumer集合、capability或保护generation不属于可随意
修改的本地策略；改变这些事实须重新接纳并分配新reservation/表示，不能承诺原wire不变。
实际编码/压缩、segment size、content type、证书和保护配置会改变 wire，必须绑定。
已证明固定的 segment count 可以预先绑定；可变输出的实际 count/bytes 则在最终签名
manifest/FinalBlock 中绑定，`<RepresentationId>` 只绑定预定分段规则。预取 count estimate/上界是预算，不是
虚构的精确表示事实；它不参与 `<RepresentationId>`，也不能被当作最终 count 接纳。
当前endpoint/manifest和密码上下文绑定了一些固定预算；保留该快照的完整绑定，不为
改本地fetch策略重新序列化它。消费者的局部策略放在原retrieval reference/获取状态中，
有效预算不超过已接纳硬界；不能只从Name hash删字段，却重算另一份密文或原包fingerprint。

公开模型/执行坐标会泄露相应元数据。prompt、token 序列、私钥、对称密钥及低熵秘密的
明文摘要不放入 Name。敏感字段留在加密内容；公开随机标识和 `<RepresentationId>` 不构成匿名性保证。

## N4. Model and Material Names

除标注为 API/schema 边界的行外，下面的对象使用受保护分段表示，**同时有** `<ObjectPrefix>/DATA/seg=<SegmentNumber>` 和 `<ObjectPrefix>/MANIFEST`。
业务 manifest 是一种 payload，其保护/分段 manifest 是另一种 Data，不能混为同一对象。
表中的“活跃”只表示基线存在 publisher，不表示拟议名称已经实现；API-only 行仅在已有调用方
实际使用时适配，不为目录完整性新增 publisher。

| 类型 | 新对象根 `<ObjectPrefix>` | Producer / 当前发布状态 |
| --- | --- | --- |
| 完整或 canonical source graph | `<Producer>/NDNSF-DI/model/<ModelPath>/source/graph/rep/<RepresentationId>` | 实际模型准备/发布者；活跃 |
| initializer aggregate | `<Producer>/NDNSF-DI/model/<ModelPath>/source/initializers/rep/<RepresentationId>` | 准备方；活跃 |
| 物理材料 bundle | `<Producer>/NDNSF-DI/material/<ModelPath>/bundle/<BundleNumber>/rep/<RepresentationId>` | 准备方；活跃。`<BundleNumber>` 是 bundle 编号，不能假称一层 |
| 材料业务 manifest | `<Producer>/NDNSF-DI/material/<ModelPath>/manifest/rep/<RepresentationId>` | 准备方；活跃 |
| assembly/material receipt | `<Producer>/NDNSF-DI/receipt/<ModelPath>/assembly/rep/<RepresentationId>` | 实际出具 receipt 的准备方；活跃；它证明特定准备结果，不等于权重身份 |
| prepared metadata | `<Producer>/NDNSF-DI/model/<ModelPath>/metadata/rep/<RepresentationId>` | 准备方；活跃 |
| canonical 业务 root manifest | `<Producer>/NDNSF-DI/model/<ModelPath>/manifest/rep/<RepresentationId>` | 准备方；活跃 |
| 当前使用的 authorization manifest | `<User>/NDNSF-DI/authorization/<ModelPath>/use/<UseId>/rep/<RepresentationId>` | 当前 User；活跃；引用稳定模型，绑定当前 grant/lease，不能当永久模型 root |
| 固定 role source/assignment | `<User>/NDNSF-DI/assignment/<ModelPath>/role/<RoleId>/rep/<RepresentationId>` | 生成 assignment 的 User；活跃；只有不含请求/租约的共享部分才可跨请求复用 |
| 普通模型 object-key capsule table | `<User>/NDNSF-DI/key-capsule/<ModelPath>/source/canonical/use/<UseId>/rep/<RepresentationId>` | 生成当前 recipient capsule 的 User；活跃 |
| StoredModel object-key capsule table | `<User>/NDNSF-DI/key-capsule/<ModelPath>/source/stored/use/<UseId>/rep/<RepresentationId>` | 当前 User；活跃；不能再伪装成 assignment |
| 单独 weights 文件 | `<Producer>/NDNSF-DI/model/<ModelPath>/source/weights/<FileId>/rep/<RepresentationId>` | 已有类型/API，未在本次 native canonical 调用链观察到独立发布 |
| 单独 tokenizer | `<Producer>/NDNSF-DI/model/<ModelPath>/tokenizer/<FileId>/rep/<RepresentationId>` | 同上；不因此新增 publisher |
| 独立发布的派生/预切片文件 | `<ArtifactStem>/file/<FileId>/rep/<RepresentationId>` | 真正生成并发布碎片的一方；现有 `ArtifactFile` 类型/API。适用条件见下文 |
| 独立发布的派生制品业务 manifest | `<ArtifactStem>/manifest/rep/<RepresentationId>` | 出具清单的一方；若现有发布调用独立提供此业务对象，使用该名字并绑定准确文件引用 |
| 分页业务 manifest | 令 `<ManifestStem>` 为相应业务 manifest 的 `<ObjectPrefix>` 去掉最后 `/rep/<RepresentationId>`，页面对象根是 `<ManifestStem>/page/<PageNumber>/rep/<RepresentationId>` | 分页编码/API 支持边界；本次 native canonical 链未观察到独立 page publisher，不据旧 inventory 的 Python publisher 记录声称仍在发布 |
| 语义输入文件 | `<User>/NDNSF-DI/input/<InputKind>/<InputId>/rep/<RepresentationId>` | 输入发布者；已有通用 API，不等于下面当前 request-input 的第二份副本 |
| 没有更具体类别的 blob | `<Producer>/NDNSF/blob/<PublicLabel>/rep/<RepresentationId>` | 原发布者；已有通用 API；不能拿它吞掉模型/activation 的语义 |

### Partition Coordinates for Derived Artifacts

同一模型的碎片应从名字看出采用何种切分、覆盖哪部分、入口是什么。
`<ArtifactStem>` 定义一次，文件与业务清单共用：

```text
<ArtifactStem> = <Producer>/NDNSF-DI/artifact/<ModelPath>/<PartitionCoordinate>/entry/<ArtifactEntry>/profile/<ProfileLabel>
```

| 切分方式 | `<PartitionCoordinate>` | 已验证的来源 |
| --- | --- | --- |
| 完整图的派生制品 | `graph/full` | 完整源图和已验证配方。原始 canonical graph 仍用上表的 `model/.../source/graph` |
| 按层流水线 PP | `pp/stage/<StageIndex>/layers/<LayerBegin>/<LayerEnd>` | 有序 stage 与连续层区间 |
| 单 stage 张量并行 TP | `tp/layers/<LayerBegin>/<LayerEnd>/degree/<TensorDegree>/rank/<Rank>` | 实际全组 TP degree 和所属 rank |
| 多 stage 的 PP+TP | `hybrid/stage/<StageIndex>/layers/<LayerBegin>/<LayerEnd>/degree/<TensorDegree>/rank/<Rank>` | 有序 stage、层区间及该 stage 的 TP degree/rank。整段执行的 stage 取 `degree/1/rank/0` |
| 自动 ONNX 两段图切分 | `graph/automatic/cut/<PrefixNodeCount>/side/<GraphSide>` | 在源图已验证的拓扑顺序中选出的前段节点数。`<GraphSide>` 为 `prefix` 或 `suffix`，真实跨图边界由配方绑定 |
| 按模型语义组件切分 | `graph/semantic/component/<ComponentId>` | 适配器声明的组件，例如 YOLO 的一个已验证子图。完整节点集与输入输出由配方绑定 |

层区间用零起点半开区间；`<StageIndex>` 取切分方案中的 stage 顺序，`<Rank>` 取所属
stage 内的 TP rank。`<ArtifactEntry>` 是适配器依据已验证入口元数据提供的单组件标签，
如 `region`、`attn-13`、`mlp-13` 或 `head`。有层内入口时须验证入口层号属于给定
层区间。stage 级整图、初始化及 head 依各自入口契约产生坐标。不能从 role URI、
输出文件名或放置时的每单元 `degree=1` 反推实际张量切片。

`<ProfileLabel>` 由已验证配置、配方和实际切片生成。它必须能区分同一坐标下支持的
不同切片方式。以 Qwen 的张量碎片为例，使用实际 KV 头和 FFN 通道的半开范围，
例如 `kv-0-4-ffn-0-1536`。完整身份仍由已认证对象记录和 `<RepresentationId>` 绑定，
包括源图及 initializer、选中节点或张量切片、入口与 I/O、配方摘要、完整 profile、
量化/布局和内容身份。若标签与经过验证的配方或实际范围不一致，则拒绝。等价输入权重
经规范化后若解析为相同切片，且其他原包绑定也相同，可复用原引用。

以下是同一模型源、同一准备方的拟议文件段名示例。前两行区分相同层区间的整段 PP 与
混合切分碎片。后两行在相同 stage、degree、rank、入口下区分均分与 `1:3` 加权切片。
源模型有 8 个 KV 头和 3072 个 FFN 通道，两行分别标明 rank 0 取得的实际范围。

```text
/lab/model-preparer/NDNSF-DI/artifact/qwen3/0.6b/int4/revision/r3/pp/stage/1/layers/14/28/entry/region/profile/whole/file/model.onnx/rep/<PpRepresentationId>/DATA/seg=0
/lab/model-preparer/NDNSF-DI/artifact/qwen3/0.6b/int4/revision/r3/hybrid/stage/1/layers/14/28/degree/1/rank/0/entry/region/profile/whole/file/model.onnx/rep/<RegionRepresentationId>/DATA/seg=0
/lab/model-preparer/NDNSF-DI/artifact/qwen3/0.6b/int4/revision/r3/hybrid/stage/0/layers/0/14/degree/2/rank/0/entry/mlp-13/profile/kv-0-4-ffn-0-1536/file/model.onnx/rep/<EqualShardRepresentationId>/DATA/seg=0
/lab/model-preparer/NDNSF-DI/artifact/qwen3/0.6b/int4/revision/r3/hybrid/stage/0/layers/0/14/degree/2/rank/0/entry/mlp-13/profile/kv-0-2-ffn-0-768/file/model.onnx/rep/<WeightedShardRepresentationId>/DATA/seg=0
```

当前原生主链发布 canonical graph/initializer 或材料 bundle，Provider 按密封配方在本地
组装 ONNX 碎片。bundle 是储存和打包单位，可能含多层或跨多个角色复用的材料；
`bundle/<BundleNumber>` 来自实际包序号。`node-<index>`、initializer、材料 byte range、
本地 `model.onnx` 和 runner entry 用已有配方、清单或本地记录关联。对本地碎片的可读标识
使用 `<PartitionCoordinate>/entry/<ArtifactEntry>/profile/<ProfileLabel>`，和实际缓存路径、
完整配方摘要一起显示。只有既有应用经发布接口独立发布派生物时才使用 `<ArtifactStem>`。
同一份原始材料可被多个配方或制品引用，按原 Name 读取。执行本地组装、换 Provider、
仅选择 CPU/CUDA 或改 Repo locator
不触发新增模型 Data。变更实际导出、切片、布局或保护时，根据涉及的语义与表示重新命名。

固定 assignment 内容若含本次 request/attempt/authorization，就必须归入当前调用表示；
不能只因名字省略 request 就声称可复用。capsule 的 recipient、provider boot、expiry、
publication/object names 等完整绑定留在受保护 table，`<UseId>` 不暴露密钥或个人身份。

### StoredModel Package

这条现有模式有 package generation、AES-GCM objects、清文 index/root 和本地私有 handle。
不把它强行套成“每个对象再发布一个 ProtectedBlob manifest”。新 package 根：

```text
<StoredModelPrefix> = <Producer>/NDNSF-DI/model/<ModelPath>/stored/rep/<PackagePublicationId>
```

| 实际 Data 类型 | 新 exact name |
| --- | --- |
| 加密 material bundle | `<StoredModelPrefix>/object/material/bundle/<BundleNumber>/DATA/seg=<SegmentNumber>` |
| 加密 material manifest | `<StoredModelPrefix>/object/material/manifest/DATA/seg=<SegmentNumber>` |
| 加密 material receipt | `<StoredModelPrefix>/object/receipt/assembly/DATA/seg=<SegmentNumber>` |
| 加密 prepared metadata | `<StoredModelPrefix>/object/metadata/DATA/seg=<SegmentNumber>` |
| 加密 canonical business root | `<StoredModelPrefix>/object/business-manifest/DATA/seg=<SegmentNumber>` |
| 加密 source-free catalog | `<StoredModelPrefix>/object/catalog/source-free/DATA/seg=<SegmentNumber>` |
| 清文 package index | `<StoredModelPrefix>/index/DATA/seg=<SegmentNumber>` |
| 清文 publication root | **单包** `<StoredModelPrefix>/root`；不是 `business-manifest`，不额外追加 segment |

包内对象角色/编号唯一；全部内容/密文摘要、exact object names、segment 几何和完整 source
身份由 index/root 链绑定，`<PackagePublicationId>` 不自引用 root 的最终 wire hash。相同 `<PackagePublicationId>` 下不能变更对象集。
现行 index 的对象 ID 包括 `material-bundle-<BundleNumber>`、`material-manifest`、
`material-receipt`、`prepared-metadata`、`business-root` 和 `catalog-<ConfigDigest>`；
按上表的角色映射迁移。加密 `business-root` 与外层单包 `/root` 分别校验。当前持久模型
从包中恢复 source-free catalog 的现行适配器范围为 Qwen/Llama。
每包当前只有一个source-free catalog，路径用固定语义 `catalog/source-free`；原内部
catalog ID/config digest仍由index绑定，不能把72-byte的catalog-hash ID当成64-byte公开label。

本地 capability/handle 保存 Producer、Repo locator、rootName、rootWireDigest、masterKey。
**handle 和 masterKey 不是公开 Data**。DigestSha256 签名只提供自一致性，不证明 Producer
身份；读取方必须通过受信任的配置/授权通道取得固定 rootName+rootWireDigest 和密钥，
并检查完整 index/object 链。第三方准备方离线后，获授权 User 可复用该 handle 的授权材料，
产生自己当前的 capsule，而不要求准备方重新上线或把主密钥公开放进名字/回复。

既有派生 key 和 AAD 绑定完整 objectName；因此更换新名字必须新建 publication、重新封装。
**旧 package 继续按旧名字读取**，不得 Repo 别名改包、原地改名或为了命名整洁批量重发模型。
原有 package journal/handle 是下次实验的优先入口。

## N5. Runtime Data Names

### Independent Objects

| 类型 | 新名字 | Producer / 关键绑定 |
| --- | --- | --- |
| request-scoped 保密输入 | `<InputPrefix>=<User>/NDNSF-DI/input/request/<RequestId>/attempt/<Attempt>/recipient/NameComponent(<Provider>)/rep/<RepresentationId>/v=0`；小输入单包 `<InputPrefix>`，分段输入 `<InputPrefix>/seg=<SegmentNumber>` | User；接收方是完整 Provider Name，不以 rank 代替 Provider。沿用现有两种输入载体，不额外发布通用 manifest |
| PP activation、DAG tensor bundle | 下述统一 tensor `<TensorPrefix>`，`<TensorPrefix>/MANIFEST` 与 `<TensorPrefix>/DATA/seg=<SegmentNumber>` | 产生 bundle 的 Provider |
| TP 跨 Provider partial/contribution | 同一 tensor `<TensorPrefix>` | 产生 partial 的 rank 所属 Provider；接收方不能改 Producer |
| decode feedback/state bundle | 同一 tensor `<TensorPrefix>`，`port` 指明实际反馈种类 | 产生反馈的 Provider；没有第三套 feedback transport |
| 可移交的受保护 KV checkpoint | `<CheckpointPrefix>=<Producer>/NDNSF-DI/checkpoint/conversation/<ConversationId>/epoch/<Epoch>/role/<RoleId>/state/<StateLayoutLabel>/rep/<RepresentationId>`；`<CheckpointPrefix>/MANIFEST`、`<CheckpointPrefix>/DATA/seg=<SegmentNumber>` | 导出状态的 Provider；`<RepresentationId>` 合并当前 encrypted-wire 与 packet descriptor 的完整事实 |
| token/application/progress/End 流事件 | `<Producer>/NDNSF/event/service/<ServiceId>/request/<RequestId>/attempt/<Attempt>/stream/<StreamId>/rep/<RepresentationId>/seq=<SequenceNumber>` | 产生事件的 Provider；token 与 End 是 Content 中的类型，共用有序流；`<RepresentationId>` 为整条 immutable stream generation |
| 最终推理结果 | N6 Core RESPONSE；大结果引用 LARGE-RESPONSE | 当前没有独立 `/NDNSF-DI/result` publisher，不新增一份副本 |

tensor 沿用当前两种 framing。普通模式发布 `<TensorPrefix>/MANIFEST` 及全部
`<TensorPrefix>/DATA/seg=<SegmentNumber>`；inline-first 模式的 **MANIFEST Data 同时装入
manifest 和 segment 0 的受保护 envelope**，不再单独发布 `DATA/seg=0`，其余 segment
名字不变。endpoint 密封的是允许的 framing 策略，最终模式由签名载荷验证；只允许在任何
包尚未发布时选择一次 framing，原包提交后不得同名切换。该策略影响表示，进入表示承诺；
预取 hint/cwnd 仍不是表示事实。不得把逻辑 segment 数误算为实际 Data 包数。

这里的新event用于已携带认证语义坐标的DI调用；普通Core调用缺少这些坐标时保留原event
规则。坐标进入StreamBinding，不通过标签猜测完整request/service授权身份。

输入保留当前 SegmentFetcher 所需的 versioned prefix 和小/大两种模式；typed `v=0`
在 publication `<RepresentationId>` 内固定，实际变更由新 `<RepresentationId>` 区分。Selection 的认证 binding 传递/唯一推导
完整 `<InputPrefix>`、模式和分段界限，Provider 更新现有重建逻辑并核对它；不能仍只按旧 attempt
拼接名字。小输入的签名包可沿用现有 inline 传递，内嵌原 wire 不另取新名字。

统一 tensor 根（所有字段位置固定）：

```text
<TensorPrefix> = <Producer>/NDNSF-DI/tensor/request/<RequestId>/attempt/<Attempt>/round/<Round>
    /phase/<Phase>/role/<RoleId>/step/<StepId>
    /layers/<LayerBegin>/<LayerEnd>/op/<Operation>/rank/<Rank>/batch/<Microbatch>
    /port/<PortId>/rep/<RepresentationId>
```

`<Phase>` 只取能提前确定的 `prefill`、`continuation`、`execute`。自回归输出的producer
epoch为0时用prefill，之后用continuation；真正DECODE与CHECKPOINT_FINALIZE由收到后
验证的lineage/manifest判定，不能把未来EOS/stop的结果或最大token数当作预取依据。
这是Name的发布阶段分类，不替代runner的执行phase，也不改变prefill/decode/finalize计时统计。
phase、层区间和step来自经过验证的adapter事实，写入sealed endpoint并由双方校验；
不能从tensor label或rank猜。通用非层式DAG用 `layers/none/none`；step-bound输出用真实
stepId，普通非step PP使用 `role-execution` 命名坐标，不创建NativeExecutionStep，也不
要求其runtime step/slot字段非空。不把per-consumer dependency index当成source step编号。
非自回归执行用 `phase/execute`（单轮时 `round/0`），
不伪造 prefill/decode 或层号。`operation` 使用计划声明的类型，
直接沿用已有 `ACTIVATION`、`PIPELINE`、`TENSOR_PARTIAL`、`TOKEN_FEEDBACK` 等值，
不新增operation别名；ATTN/MLP由真实step/layer区分。
TP 每层本地 SUM 仍在本地算，不为给它命名而发布新的 SUM Data。若 SUM 结果确实跨节点，
它就是计划中声明的另一个 outgoing tensor endpoint。

Spec252 已允许同一密封 TP partial 有多个获准消费者，也支持下一 stage 在本地归约。
以 TP2→TP1 为例，下一 stage 从两个上游 rank 的**各自 Producer** 获取末层 FFN partial，
本地 SUM 后把结果作为 `LOCAL_STEP` 输入交给 `REGION`。只有同一个 partial 实际还有其他
接收者时，他们才共享该 Producer 的同一份 manifest 和 segments。每个接收方的 step/slot
由各自密封投影校验。同一已接纳受众内不因接收方身份或 slot 另造包名。改变已获准受众、
保护或布局需要按 N3 重新接纳。能力条件不足时保留已有兼容计划和实际 Producer 路径。

`<Round>` 统一表示**产生该对象的inference epoch**，不等于packed operation index。
设 `packedIndex=endpoint.round`、`stride=namingOperationStride`：stride>0时
`targetEpoch=packedIndex/stride`，`operationOrdinal=packedIndex%stride`；反馈是
`producerEpoch=targetEpoch-1`，其他输出为`producerEpoch=targetEpoch`。反馈targetEpoch
必须大于0。Name的Round取producerEpoch；consumer仍按原targetEpoch接纳。非生成DAG
的Round为0，原operation index仍完整绑定于step/metadata及表示承诺，不能被丢弃。
既有per-edge operation ordinal若改变实际KDF/保护上下文，绑定在representation的保护
事实中，不进入共享producer-step语义键；不为命名而跨不同capability合并授权组。
因此epoch0产出的首个feedback名为round/0、phase/prefill，消费它的是epoch1。

consumer 从已接纳 endpoint 推导 exact manifest/segment names，先发有界预取 Interest；
收到 manifest 后验证完整执行绑定/lineage、Producer/签名授权、layout、保护和分段承诺，再
接纳 payload。推导的 segment count 只是预取界限，不能替代最终 manifest 校验；
超出已接纳的 shape/预算承诺则取消 pending Interests 并拒绝；estimate 与合法实际 count
不同只收缩/补充有界窗口，不因 estimate 不准拒绝合法对象。无需把每个 activation name 逐个
塞入大 manifest：声明共同 stem、范围/规则、必要摘要即可，完整集合必须可唯一重建。

### Embedded Records

| 业务内容 | 载体与 Producer；不另建对象族 |
| --- | --- |
| conversation ready、receipt、commit、rollback | 现有 COLLAB；发送该事实的 User/Provider |
| conversation control、lease、recovery-activate、recovery-cancel | 同上；topic 区分目的，Content 绑定 conversation/epoch/role |
| execution cancellation、cancellation-proof | 同上；发起取消者/给出证明者 |
| handoff Begin/Poll/Grant/Commit/Cancel | 当前 targeted Request/Response；各自消息发送者 |
| save/withdrawal query、confirm、status | 同上；Provider 回复自己的状态 |
| readiness、execution activation、selection status | 现有对应 signed RPC 回复；被查询/接收控制的一方 |
| 模型驻留、已缓存对象及链路观测 | Provider 的 ACK（原生 Offer V4），随能力字段验签和接纳；驻留查询索引归本地缓存 |
| 最终 descriptor、collective capability、sealed plan/role projection | 保留在 assignment、COLLAB、Response 等已有消息，不凭 schema 名称新增独立 Data |

### Examples

以下是**拟议名称**，`<RepresentationId0>/<PackagePublicationId0>` 是一个组件的显示占位符，不能直接拿来当合法 32-byte `<RepresentationId>`。
例子中的 Producer 都是完整前缀，rank 只是随后字段。

```text
/lab/model-preparer/NDNSF-DI/model/qwen3/0.6b/int4/revision/r3/source/graph/rep/<RepresentationId0>/DATA/seg=0
/lab/model-preparer/NDNSF-DI/material/qwen3/0.6b/int4/revision/r3/bundle/12/rep/<RepresentationId1>/MANIFEST
/lab/model-preparer/NDNSF-DI/model/qwen3/0.6b/int4/revision/r3/stored/rep/<PackagePublicationId0>/root
/lab/user1/NDNSF-DI/key-capsule/qwen3/0.6b/int4/revision/r3/source/stored/use/use7/rep/<RepresentationId2>/MANIFEST
/lab/provider0/NDNSF-DI/tensor/request/request7/attempt/1/round/0/phase/prefill/role/stage0/step/role-execution/layers/0/14/op/ACTIVATION/rank/0/batch/0/port/activation/rep/<RepresentationId3>/DATA/seg=0
/lab/provider0/NDNSF-DI/tensor/request/request7/attempt/1/round/1/phase/continuation/role/rank0/step/layer0-attn/layers/0/1/op/TENSOR_PARTIAL/rank/0/batch/0/port/attn/rep/<RepresentationId4>/MANIFEST
/lab/provider1/NDNSF-DI/tensor/request/request7/attempt/1/round/0/phase/prefill/role/stage1/step/role-execution/layers/14/28/op/TOKEN_FEEDBACK/rank/0/batch/0/port/next-token-state/rep/<RepresentationId5>/DATA/seg=0
/lab/provider0/NDNSF-DI/checkpoint/conversation/conversation9/epoch/2/role/stage0/state/qwen-kv-v1/rep/<RepresentationId6>/MANIFEST
/lab/provider1/NDNSF/event/service/qwen-generate/request/request7/attempt/1/stream/output/rep/<RepresentationId7>/seq=1
```

PP stage 输出可覆盖整个 stage 层区间；TP partial 的层区间只指本次操作涉及的层。
名字不需要声明“INT4 activation”：权重量化等级不等于 activation dtype，dtype/shape/layout
必须在 endpoint/manifest 中如实验证。

## N6. Retained Protocols and Local References

### Invocation Messages

当前 REQUEST/ACK/SELECTION/RESPONSE 已有语义，**保留原 grammar**。`<ServiceName>` 和 `<RequestName>`
是当前 contextual parser 识别的 service/request Name；`NameComponent` 表示已有单组件 URI 编码。
不能不带调用上下文，仅凭任意 service/request 字符串中出现 marker 就拆分身份。

| 类型 | 保留的实际名称 / Producer |
| --- | --- |
| REQUEST | `<User>/NDNSF/REQUEST/<ServiceName>/<RequestName>`；User |
| ACK/offer | `<Producer>/NDNSF/ACK/NameComponent(<User>)/<ServiceName>/<RequestName>`；Provider |
| SELECTION | `<User>/NDNSF/SELECTION/NameComponent(<Provider>)/<ServiceName>/<RequestName>`；decision 分支另加普通十进制 `/<Attempt>` |
| RESPONSE/最终结果 | `<Producer>/NDNSF/RESPONSE/NameComponent(<User>)/<ServiceName>/<RequestName>`；Provider |
| LARGE-RESPONSE | `<Producer>/NDNSF/LARGE-RESPONSE/NameComponent(<User>)/<ServiceName>/<RequestName>/<ObjectId>/v=<GeneratedVersion>/seg=<SegmentNumber>`；Provider，Response 引用原 exact object |
| COLLAB 业务消息 | `<Producer>/NDNSF/COLLAB/<RequesterNameWithCount>/<BoundRequestId>/<KeyScope>/<TopicComponentCount>/<TopicName>/<SequenceNumber>`；真实发送者 |
| 大 COLLAB，旧 tensor 兼容 | 上述根加 `/large/v=<GeneratedVersion>/seg=<SegmentNumber>`；已有 publishLargeNamed 分支按 caller 指定的 Provider-owned exact root 分段 |

ACK/RESPONSE builder 没有独立 attempt，并不单独证明当前路径已经覆盖同名不同内容。
设计采用明确的**调用级不可变契约**：同一 `<RequestName>` 的 Request、某 Provider 的 ACK、最终
Response 各冻结一份原始包；重传返回原 wire，不重新生成时间戳/密文/签名。Selection
decision 的不同 attempt 有现有独立后缀。进度/状态更新用已有 RPC/COLLAB 的新 nonce/
sequence，不把同一 Response 改成动态状态槽。一个调用只接纳一个终态；冲突结果拒绝，
不能只换 `<RepresentationId>` 就覆盖终态。若重试必须更换已有 Request/ACK/Response 的字节或其安全绑定，
先结束旧调用，再由现有 requester 分配**新 `<RequestName>`**，保留上层 conversation/恢复关联并重新
接纳。这项约束的执行须核查原包缓存和重试 owner，不凭 builder 审查宣称已经实现。

COLLAB 的 topic 继续表达 conversation/execution 操作。例如
`/ndnsf-di/conversation/receipt`、`/ndnsf-di/conversation/control`、
`/ndnsf-di/execution/cancellation-proof`；conversation/epoch/role 在受保护 Content 中严格
绑定，不把任意 role Name 拼接进 topic 造成路径歧义。对端名字是角色中的完整 User/
coordinator，实际发送者仍决定 Producer。这里仍是一个 COLLAB 实现，不是第二套控制协议。

正常发现的四个逻辑消息与 targeted 的消息序列不变，也不新建 ACK/Response 的 `<RepresentationId>` 发现
协议。Core 中引用新 DI input/tensor/model 时同步更新引用和接纳路径；消息中引用另一份
Data 必须用其真实 exact name，不据当前 actor 重建别人的名字。

### Control, Security and Repo

此表是完整性目录，名称中的 `Digest` 占位符表示当前的 32-byte Generic 摘要组件；不是全部改名。

| Data 类型 | 保留的 exact grammar / 发布者 |
| --- | --- |
| RPC reply | `<Producer>/NDNSF/RPC/<Operation>/<CallDigest>/<Nonce>/params-sha256=...`；Data **保留完整 Interest Name**，`<Producer>` 是回复内容生成者 |
| RTT/ready/goodput probe reply | 上式 `<Operation>` 为 `PROBE-RTT/PROBE-READY/PROBE-GOODPUT`；goodput packet 索引绑定于调用参数和 `<CallDigest>`，不假定另有 `seg=` |
| security/encryption profile | `<Producer>/NDNSF/SECURITY/name=encryption-profile/<ProfileDigest>`；实际提供该加密能力的一方 |
| hybrid key envelope | `<Producer>/NDNSF/KEY/kind=HYBRID-MESSAGE/SCOPE/SERVICE/<ScopeDigest>/EPOCH/<EpochDigest>/OBJECT/<CiphertextDigest>/seg=<SegmentNumber>` |
| native key grant | `<User>/NDNSF/GRANT/kind=KEY-GRANT/RECIPIENT/<RecipientDigest>/SCOPE/MODEL/<ScopeDigest>/EPOCH/<EpochDigest>/OBJECT/<GrantDigest>`；User 发布外层 exact Data，内层授权证明由实际制品 Authority 签发 |
| deployment DEFINITION | `<Producer>/NDNSF/DEPLOYMENT/name=<ApplicationLabel>/<DeploymentDigest>/DEFINITION/<PayloadDigest>` |
| deployment ACTIVATION | 同 stem `/REVISION/<RevisionDigest>/ACTIVATION/<PayloadDigest>` |
| deployment REVOCATION | 同 stem `/REVISION/<RevisionDigest>/REVOCATION/EPOCH/<Epoch>/<PayloadDigest>`；`<Producer>` 是该记录真实签发方 |
| Repo 原始 Data | 原 Producer 的上述任一种原 Name/wire/signature；不生成替代名字 |
| Repo 管理结果 | Repo 作为 Provider，复用 REQUEST/ACK/RESPONSE/LARGE-RESPONSE；**不是**另一种 `/repo/status` 对象 |

RPC operation 完整集合：`PROVIDER-READY`、`EXECUTION-ACTIVATE`、`SELECTION-STATUS`、
`USER-PERMISSIONS`、`PROVIDER-PERMISSIONS`、`POLICY-MANIFEST`、`POLICY-STATUS`、
`SERVICEACCESS`、`SERVICEPROVISION`、`CERTBOOTSTRAP`、`AA-READY`、`FRONTIER`，加上述三类 probe。
同一 RPC Interest 的回复必须冻结；新状态查询使用新 nonce，不能复用旧名字改变状态。
Data 所在 target namespace 只能由有该 namespace 发布权的真实回复方生成，不能转发
另一方未经改变的声明并冒认其 Producer。

Repo 操作完整集合：`USER_STORAGE_GRANT`、`USER_STORAGE_USAGE`、`USER_STORAGE_LIST`、`USER_STORAGE_DELETE`、
`STORE`、`STORE_RANGE`、`COMMIT_RANGES`、`ABORT_RANGES`、`INSERT`、`STORE_MANIFEST`、
`FETCH`、`FETCH_RANGE`、`MANIFEST`、`DATA_RECEIPT`、`DATA_LOCATION`、`INVENTORY`、
`CAPABILITY`、`CACHE_STATUS`、`STATUS`、`CATALOG_STATUS`、`CATALOG_SNAPSHOT`、`CATALOG_DELTA`、`CATALOG_LOOKUP`、`DELETE`。
各操作的 payload 类型并不增加独立 Data 名称族。
持久文件的 `STORE_RANGE`、`COMMIT_RANGES`、目录查询、提交回执与本地 facade 句柄属于
既有存储或管理边界。按名称检索模型/检查点原包时使用确切 Producer Name 和原签名；
Repo 节点、route、group/operation ID、目录键或回执均不进入这些对象的原名。

security profile、grant、key envelope 的 recipient/scope 摘要保留：它们绑定安全/隐私范围，
不像模型层号那样需要全部公开。验证不仅比对字符串和签名，还要检查当前 permission、
signer 对 namespace 的授权、recipient、epoch、lease、plan/request 绑定。

### Stream and External Protocols

通用 Stream API 保持现有 grammar。令
`<StreamPrefix>=<Producer>/NDNSF/STREAM/name=<StreamLabel>/<StreamIdDigest>/EPOCH/<Epoch>`，`<StreamMapPrefix>=<StreamPrefix>/MAP`：

| 类型 | 当前保留名称 |
| --- | --- |
| map block | `<StreamMapPrefix>/v=<MappingVersion>/seq=<BlockNumber>` |
| predictive content packet | `<StreamMapPrefix>/DATA/v=<MappingVersion>/seq=<SequenceNumber>` |
| ordinary semantic sample | 由经过验证的 map entry 引用实际 Producer 的原样 Name；没有一条可取代全部业务 sample 的通用固定后缀 |
| predictive group commit manifest | `<StreamMapPrefix>/GROUP/v=<MappingVersion>/<GroupId>/MANIFEST` |
| FEC repair packet | `<StreamMapPrefix>/GROUP/v=<MappingVersion>/<GroupId>/REPAIR/<RepairIndex>` |
| frontier snapshot | `<StreamPrefix>/FRONTIER/SNAPSHOT/<FrontierDigest>`；frontier query reply 用 RPC `FRONTIER` |
| stream descriptor（嵌入） | 已有打开/调用消息中的 descriptor，包含 `<StreamMapPrefix>` 与 stream binding；未发现独立固定 descriptor Data publisher，不为“补全目录”新建一个 |
| SVS outer Data | `<Actor>/<ActorKind>/<Session>/<GroupPrefix>/[typed timestamp，v3]/seq=<PublicationSequence>`；`<ActorKind>` 是字面 `user` 或 `provider`，按库分段时加 `v=0/seg=<SegmentNumber>` |
| SVS 包内业务 inner Data | 第 6.1 节逻辑消息名字；库分段时其自身也可能带 `v=0/seg=<SegmentNumber>`；不能把 inner 名当成唯一线上名 |
| NDN certificate | `<SubjectIdentity>/KEY/<KeyId>/<IssuerId>/v=<Version>`；保持标准；签发者须有相应 issuer/namespace 委托，不能机械改为 Provider 前缀 |
| NAC-ABE public parameters | `<Authority>/PUBPARAMS/<AbeMode>/v=<Generation>`；`<AbeMode>` 是字面 `CP-ABE` 或 `KP-ABE` |
| NAC-ABE decryption key packets | `<Authority>/DKEY/<EncodedRecipientKeyName>/v=<GeneratedVersion>/seg=<SegmentNumber>` |
| NAC-ABE encrypted content-key packets | `<Producer>/CK/<RandomId>/ENC-BY/<PolicyOrAttribute>/seg=<SegmentNumber>` |
| NDNSD discovery inner | `NameComponent(<NodeName>)/NDNSD/discovery/v=<GeneratedVersion>`；外层由该库的 SVS 承载 |
| NDNSD service-info inner | `NameComponent(<NodeName>)/<ServiceName>/NDNSD/service-info/v=<GeneratedVersion>`；可选外部库约定，第一组件是编码后的完整 node URI |

外部协议的 session、sequence、参数摘要、证书 key-id 等不按 DI 审美重新命名。
这些行来自安装头和当前 sibling 依赖源的核对，不属于本文固定仓库 SHA；包括
`ndn-svs/ndn-svs/svspubsub.cpp`、`NAC-ABE/src/{attribute-authority,producer}.cpp`、
`NDNSD/ndnsd/discovery/service-discovery.cpp`。它们的维护库仍是 wire grammar 的权威。
NDNSD 的实际旧名字与新应用对象的 producer-prefix 规则不同，属维护库边界；不得把它的
单组件 node URI 宣称为展开的 Producer 前缀，也不因本设计去修改可选库。
部署配置要把 actor/group 的协议 namespace 与 Producer Data namespace 正确路由；名称设计
不授权修改 NFD、ndn-cxx、SVS、NAC-ABE 等外部库。

### Local and Embedded References

以下均保持自己的本地/嵌入状态，不新增 namespace：model alias snapshot、local model digest、
完整 plan 与 projection、admission/resource/reservation callback、DI API progress callback、
conversation journal/transcript、KV 私有导出文件和内部页、底层encrypted-byte reference.objectName、StoredModel 私有 handle/masterKey、
`/NDNSF/HYBRID-KEY/...` 私有 key identifier、`MODEL-CACHE/.../OBJECT/...` 与 `RANGE/...`
文件系统引用、artifactRoot/candidate/manifest/role 的本地 prefetch alias、logical mapping root。
Spec253 的 `asset-manifest.json`、安装收据、fixture 路径和模型资格选择器也是本地输入，
它们可以验证或定位准备好的资产。它们没有应用 Data publisher，因此不分配 NDNSF 名称。
`assignedArtifact` 的 role/manifest 别名区分本地调度与缓存身份，实际获取使用独立的
`artifactDataName` 或经过接纳的 StoredModel root。
一个字符串“长得像 Name”，或者 category whitelist 接受 `plan/result/collective`，都不证明
当前存在这类独立发布。维护 API 若允许应用自定义 producer-owned semantic sample Name，
目录只能规定约束和验证契约，不能枚举无限的应用自定义后缀。
