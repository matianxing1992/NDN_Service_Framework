# Integration Contract: Production Naming Call Chains

**Status**: DRAFT / NOT_ACTIVE
**Baseline**: `spec253@cd9246fdc18007b32e45f0c742a9ed98f33f8976`
**Design binding**: FR-001–FR-009；名称只由 [naming.md](naming.md) N1–N6 定义。

本文件中的字段/改动为 planned，已有函数签名为 existing。实施前在当前源码核对符号；
不能把历史行号当作新分支接线证据。所有校验均适用于 Producer、cache、Repo 三种来源。

## C1. Shared Metadata and Component Builders

复用 `ndn-service-framework/ProducerDataNaming.hpp/.cpp` 的 `makeProducerDataPrefix`、
`makeNamingDecimalComponent`、`makeNamingDigestComponent`、`makeReversibleNameComponent`、
`computeNamingDigest` 和 Name wire 限制。保留这些函数签名，不引入第二套 digest transcript。
`NameComponent(<Name>)` 对应已有可逆 helper，不是新的 API。
`categoryAllowed` 显式登记本目录新增的 `artifact`、`authorization`、`key-capsule`、
`checkpoint`，同步各自parser和ProducerSignerPolicy的允许范围；不放宽为任意category。

`PublishedContentName` 继续拥有静态内容的语义元数据。保留其 `contentPrefix(producer, plaintextDigest)`、`objectIdentity()`、`encode/decode`
API，更新其中的字段校验和编解码：

| Field group | Source / planned change and validation |
| --- | --- |
| existing `kind/modelIdentity/artifactKind/coordinate/profileIdentity/inputKind/label` | 保留完整身份；`label` 对新规则只作完整公开字段，不再调用截断 helper。旧 `ArtifactFile` 的 `artifactKind/coordinate` 是单组件，分别走旧 codec。新派生对象经已有 `PublishedContentName` 校验并逐字段编码 N4 的 `<PartitionCoordinate>`，不复用任意字符串拼出路径 |
| `semanticRole` | 增加受 `kind` 约束的业务对象角色：source graph/initializers/bundle/metadata/business manifest/receipt/authorization/fixed assignment/capsule 等，完整集合对应 N4 各行；不得从 label 猜角色 |
| `modelFamily/modelSize/quantization/sourceRevisionLabel` | 从准备后已验证 model descriptor/config projection 获得，组成 ModelPath；缺失事实显式 unspecified，完整 modelIdentity 仍必需；Core 不识别 Qwen 型号字符串 |
| `bundleIndex/profileLabel/useId` | 分别仅在 bundle/artifact/当前授权或 capsule 行要求；缺失、额外或与实际对象不符则拒绝。复用 label 作已有 file/role/public ID，不再另添同义 ID |

### Derived artifact source and admission

现行链路为 `NativeRequestCatalog` → Qwen/Llama 层区间、Qwen TP/hybrid、YOLO 语义组件或
automatic ONNX 图分区 → `NativeCanonicalRolePreparer` 将具体角色投影到源图节点和 I/O →
`NativeOnnxAssemblyWorker` 生成绑定源图、材料、配方、入口、profile 和实际切片的 recipe
digest → `NativeCanonicalOnnxAssembler` 读取原始材料并在 Provider 的 recipe cache 组装
ONNX 文件。Qwen 加权切片以 `tensorShardV1` 提供 `kv-group` 和 `ffn-channel` 实际范围。
单元放置所见的 degree 和文件 basename 无法替代这些事实。对 automatic ONNX，分界取
`automatic-onnx-split.cpp` 的已经验证的拓扑 cut 和 prefix/suffix 映射；保留跨边界的全部
张量和 KV 对。对 YOLO，使用 `NativeYoloPlanner` 的实际组件和节点归属。
将 N4 的本地可读标识放入现有组装记录，随实际 recipe digest 与路径提供。沿用已有
缓存路径及判据，供排查时从记录识别切分、角色和入口。

如果已有调用独立发布派生文件，由相应 DI adapter/准备方从这些事实投影 N4 的坐标、入口、
可读 profile 和完整 recipe/profile 身份。`PublishedContentName` 负责通用字段的闭集与
wire 编码。C2 绑定完整源图、配方、I/O 及 bytes 的 commitment，并将准确文件名写入认证
引用。读取时，`ProtectedBlobManifest` 和制品接纳方根据已验证元数据核对名称、Producer、
坐标与完整对象事实后再取/解封原包。坐标与配方不一致、入口出界、同标签下的不同范围
或旧/新 schema 混搭，均在发布或接纳边界拒绝。仅支持语义命名的通用类型或薄 API 时，
沿原 codec/fixture 验证，保持本地组装和原模型发布调用边界，不新增制品网络发布者。
现有 canonical/StoredModel 只将实际 bundle 序号投影给其名字；bundle 内的节点、
initializer 和 byte range 仍从已验证材料清单解析。

`contentPrefix` 返回 N4 中 `/rep/<RepresentationId>` **之前的语义 stem**；表示尾由现有
protected publisher 构造一次。`plaintextDigest` 不再堆成多个路径 hash，但仍进入经过
验证的 object identity/representation descriptor。该 API 行为变化的所有 callers、
manifest validator 和绑定在同一单元迁移；不是只改一个函数返回字符串。

不要全局改变 `makeNamingLabelComponent` 的既有行为影响 UAV/Stream/外部格式；新 DI
语义字段在其生产 owner 明确验证 1–64 UTF-8 字节后直接追加一个 Generic 组件。
可以复用内部检查，不能为此创建第二套命名框架。既有实际包的 legacy reader 不调用新
builder 反向改写原名。所有新增字段均进入 codec/identity；不接纳“只改显示”字段。

保留**两个闭集记录形状**：带完整`semanticRole`及其必需坐标的新语义记录，和不含这些
新增字段的既有canonical记录。新记录严格重建新Name；旧记录使用保留下来的原canonical
encode/objectIdentity/prefix算法，缺失新字段时旧序列化字节/摘要不变。不能让新builder
重建旧Name。`DataNameReadMode::AllowLegacyRead`原本只放行更早的LARGE-DATA/receipt
路径，不能把它误当成此处旧canonical算法。按已验证记录字段及expected exact reference
选择唯一codec，结构错误或Name不符直接拒绝，不允许parse失败后逐种格式试探。

现有User/Provider已在完整Producer的NDNSF/NDNSF-DI前缀服务IMS并注册身份路由
（ServiceUser:2936–2952；ServiceProvider:4191–4211）；新类别复用该serving/filter和原
prefix-role配置，不按类别另造路由。真实远端读取仍要确认Data Interest到达原包owner，
Repo locator只作转发信息，不改变Producer Name。

## C2. Representation Construction and Protected Publication

现有 `ServiceUser.cpp::makeProtectedBlobDataPrefix` 接收 Producer、content digest、key
reference、epoch、scope、publication identity、freshness、segment bytes、contentName；
保留其输入职责和返回的原包保护描述，按 N3 用一个 RepresentationId 替换串联摘要。

复用 tagged `NamingDigestField` transcript，按递增 tag 固定以下语义字段：

| Tag | Kind / source | Commitment boundary |
| --- | --- | --- |
| 1 | NameWire / admitted Producer | 完整 Producer，不能只是 rank 或可读尾部 |
| 2 | UTF8 / object category | N4/N5 的实际对象类别和业务角色 |
| 3 | CanonicalJSON / semantic identity | 已验证模型/对象事实；已知 material 的 plaintext identity；不含自身 Name、最终 manifest hash |
| 4 | Bytes / publication reservation | 现有发布 owner 持久化的随机 seed；runtime 的来源见 C4 |
| 5 | CanonicalJSON / representation descriptor | encoding、固定 segment bytes、framing policy、content type/freshness 等 wire 配置；实际可变 count 在最终 manifest |
| 6 | CanonicalJSON / protection descriptor | cipher、key reference/epoch/scope/recipient、既有group/operation reservation和nonce policy；不含密钥、后选nonce或含R的final capability digest |
| 7 | CanonicalJSON / signer context | 证书/签名配置和必要授权绑定，不含最终随机 signature value |
| 8 | CanonicalJSON / runtime source facts, runtime only | C4 的 producer-output 执行事实；不存在 runtime 时不伪填执行字段 |

domain 使用现有项目 naming domain 机制区分静态对象、runtime output 和 package；固定
tag/kind/顺序在生产 serializer 和独立 oracle 中对照，不增加路径版本或第二套 JSON 排序。

调用链：`NativeCanonicalArtifactPublisher::publicationContentName/publicationManifestSuffix`
→ `ServiceUser::publishEncryptedLargeDataImpl` → 名字/AAD 已确定后加密分段签名
→ `ProtectedBlobManifest::getName/encode` → 当前原包发布/Repo store
→ `fetchAndDecryptLargeDataUntil` → `ProtectedBlobManifest::decode/validate`
→ canonical assembler 消费验证后的源对象。

`ProtectedBlobManifest` 新名字由统一 stem 重建 `ObjectPrefix/MANIFEST`，不再额外追加
最终 payload hash。manifest 保存已验证语义字段与表示 descriptor，严格重算 root、
segment prefix 和权限；payload digest、geometry、AEAD 完整性依然校验。旧包仅由其
实际保存的 exact reference/read mode 接纳，禁止宽松解析失败后尝试另一安全规则。

新语义 manifest 在 `PublishedContentName`（TLV 157）后追加公开 publication nonce
（TLV 159，12 字节）和签名证书 Name（TLV 161，完整 Name wire）。nonce 用于重算
tag 4 并核对加密 envelope，证书的 canonical Name 用于 tag 7。签名验证通过后，核对
实际包的 KeyLocator 与证书或证书所属 key 的 Name。没有语义坐标的旧 manifest 保持原
字段集合和原包名称。

新发布的 `NativeCanonicalArtifactPublisher::publicationManifestSuffix` 不再产生root/
material-manifest/receipt的旧override：统一让Core从semantic stem和表示尾生成MANIFEST，
传入空manifestSuffix，不另发布一个adapter侧别名。旧suffix仅随旧记录读取/重放。
durable lookup也必须一起改：先查并验证已有receipt/原包，再决定复用或新建reservation；
复用时返回旧记录的真实dataPrefix/manifestName，不先造新`<RepresentationId>`再要求旧包与新Name相等。
`ServiceUser.cpp:8071–8077`当前contentName.encode/suffix逐字节比较仅用于同记录形状；
跨形状复用须比较验证后的Producer、完整模型/对象/配方事实、payload digest/bytes及权限，
并保存原记录codec。不能只比可读label，也不能为了通过比较而改写旧manifest。

capsule还要改真实接收入口：`nativeInstallModelObjectKeys`当前把两类table都交给
`nativeValidateFixedRoleSourceProducer`，后者只认assignment旧后缀。新路径按admitted
context先验证User、key-capsule类别、canonical/stored来源、ModelPath和UseId再获取；
fixed assignment继续由自己的validator验证。获取/解密后仍检查schema、plaintext digest、
recipient证书、Provider boot和expiry。补fixed-vs-capsule、两种capsule互换负例，并走
install→真正object fetch，不以table publisher/helper测试代替。

发布 owner 保留 reservation 与**原始签名 wire**，提交前检查全批包长度/容量；发布失败
不得对同名改 framing/保护后继续。重传服务原包；清理依旧由现有 publication/lease owner
负责，不创建无限名字历史。不同内容或重封装分配新表示，不返回伪成功 receipt。

模型/authorization/fixed assignment/capsule 的 owner 分别沿 publisher、
`NativeInferenceClient`、`NativeExecutionPlanJson`、`model-object-keys.cpp` 接线。
模型事实来自准备结果；当前 useId/grant/Provider boot/expiry 来自当前调用，不能写进
永久模型身份。capsule 两种来源有明确 semanticRole，不能继续伪装成 assignment。

## C3. StoredModel and Durable References

保持 `NativeStoredModelReference/Handle/Root/Object` 的职责，复用 `stored-model-preparation`
→ `nativePublishStoredModel` → journal/seal/index/root → Repo → read/open 的现有链。
`nativeStoredModelObjectName(publicationPrefix,id)` 保留函数签名，按经过验证的 object role/
编号投影 N4 StoredModel 名字；禁止把任意 id 作为多组件路径展开。

默认prefix在 `PreparationJournal` 内、bindPrefix/loadCatalog之前分配；现有context只有
Producer/Repo/publicationPrefix/modelDigest/catalogId/manifestId，不能从digest反推ModelPath。
因此preparer从已验证source-free configuration提取 `semanticPublication`（ModelPath字段和
package保护事实），在调用原factory/journal**分配prefix之前**传入；由同一journal锁内
读取已有state，存在则验证/复用，否则分配seed和prefix，并一起持久化。不能为取得Name
再次加载/导出模型。显式传入的prefix也要与该context/Producer一致，不绕过默认验证。
具体改动是给原journal-aware `nativePublishStoredModel` overload增加一个具名的
`NativeStoredModelSemanticContext`参数，仅含C1四项ModelPath事实；preparer传入已验证
值，package保护事实由原publisher提供。其他native重载同步转发明确的unspecified事实
或真实来源值，不能虚构size/quant。journal在原context内保存semanticPublication与seed，
没有第二个state owner；prepare/preflight与显式prefix检查也消费同一context。

journal 在生成第一个对象前保存新 package prefix、PackagePublicationId、模型/保护事实
和 masterKey。seal、object key derivation、AAD、segmenter、catalog/index、public root、
rootWireDigest pin、能力读取/命名校验一起迁移；新 object 和 index 采用 N4 exact prefix
并使用已有 `readSegments` → `NativeStoredModelPacketIo::fetch` 的 exact segment
读API；它按给定prefix加typed segment并验证原包，不依赖SegmentFetcher的version探测。public root 仍为一个单包，business root 仍是另一份加密对象。

原 handle 包含真实 rootName，恢复根据持久保存的完整 reference 使用对应 read contract，
不改名、不从显示 label 重构 root。准备方离线后 User 从已受信任的 handle/授权材料生成
当前 capsule；不公开 masterKey，也不新建在线密钥服务。DigestSha256 原包不能替代
可信 root pin。既有对象引用被其他新 manifest 引用时，保留它们自己的真实 Producer。

新journal/index/root携带已验证的semanticPublication记录，新索引按已声明角色投影对象名。
旧记录没有这个字段，按闭集旧record contract校验其原prefix/entries，而非调用改过的
`nativeStoredModelObjectName`（现有journal在623/637行会重算名字）。已完成旧journal返回
原handle/原包；部分旧journal仅以保留的原codec及原cipher/packet grouping继续原样恢复，
不升级成新Name。缺失必需原数据或context冲突则不修改旧state地报错。格式选择来自已
保存/经root pin验证的记录，而非无鉴权的字符串前缀猜测或“宽松legacy”fallback。

**Failure/restart**：journal 已完成的原包直接复用；current 对象只恢复原密文/分组。
无法恢复时报告新发布需求，不以旧 rootName 重新生成不同签名包。新进程重开 Repo/store
后再读取原包，字节相同才构成持久复用证据；写入方内存命中不算。

沿 `model-repository-publication.cpp`、`stored-model-repository` 和 native KV 的 `RepoClient`
facade 跟踪提交与读取。持久导入/范围写入及 operation receipt 负责落盘；认证 handle、
index 和 packet descriptor 继续固定原名、digest 和 Producer。原包路径验证实际返回的
原 wire。管理服务、注册目录、Repo target 和 receipt 不成为模型或检查点的命名来源。
准备方离线重开测试覆盖这些边界。跨配置复用须核实相同的已接纳模型/制品承诺及原包引用。

## C4. Runtime Projection, Exact Names and Input Admission

保留 `tensorObjectNamePrefix/ManifestName/SegmentName`、
`makeTensorSegmentDataName` 的 API；在现有 `NativeTensorEndpointV3`、`DependencyEdge`
及 fixed template/invocation binding 中传递以下 planned 信息，不另建 endpoint owner：

| Fields / owner | Rule |
| --- | --- |
| public request/conversation ID | 由现有请求/会话 owner 分配或无损提取已存在公开随机标识，并在原绑定中映射完整 request/conversation 身份；没有全局 alias 查询表 |
| `namingPhase/namingStepId/layerBegin/layerEnd/outputPort` | 由已验证adapter plan/step/role事实投影；PP用stage范围和role-execution坐标，TP用实际step/层，generic DAG用execute/none；不改变runner执行phase或强加PP step模板 |
| existing Producer/role/rank/round/stride/microbatch | 继续验证源事实和 operation stride；一个 output 在同一执行坐标只发布一次 |
| invocation publication seed / protection generation | User/planner 接纳 ACK 后，在现有 binding 密封；固定 assignment 只含可复用模板，不放本次 seed/授权 |
| producer-output identity | 当前 request/attempt/epoch、源侧程序/artifact/step、output slot、layout/security；不含自身 Name/`<RepresentationId>`、最终 endpoint/plan seal、outgoing consumer-specific slot 或获取策略 |
| consumer edge admission | target step/slot、完整final plan、dependency lineage和权限仍在密封edge/manifest校验。`consumerRoles`在一个output representation的publisher快照中冻结为完整允许集合；同保护引用该值，单个consumerRole只用于本地edge接纳。不同保护operation不能冒充同一原包 |

**Request/attempt来源**：NativeInferenceClient的公开logical requestId保持稳定，Core
coreRequestId在初次相等、replacement时换成新的transport identity；planner、StreamBinding、
RequestSecurityBinding、End/Response和恢复校验仍用当前Core身份。Name的RequestId采用
公开逻辑ID的单组件表示，在原invocation/projection/StreamBinding中认证绑定
`(publicRequestId, coreRequestId, attempt)`；不改写AAD中的Core身份，不解析`/recovery/`
字符串猜映射。没有公开DI身份的普通Core调用保留现有Core event规则，不另造alias表。
首次attempt1；只有原allowReplacement/maxReplacements及失败/剩余预算guard允许时进入2，
并更新Core identity和streamEpoch，拒绝attempt1迟到事件。warm新请求仍是attempt1。

**TP tuple桥接**：从Catalog role expansion和NativeDependencySpec，通过fixed template/
projection，到runtime DependencyEdge必须唯一保留并交叉校验
`(dependencyId, producerRole, producerStepId, producerSlotId, consumerRole, consumerStepId, consumerSlotId, operationIndex, epoch)`。
从已认证端点取 `dependencyId`，复用现有 `NativeEdgeEndpointBinding` 的完整 tuple。
`runtimeScope` 和 `transportScope` 保持各自用途。
endpoint新增语义字段与原dependency按完整tuple
关联，不能仅按两端role或tensor label连接；一个source output的多条consumer edge各保留
自己的target binding。现行 `roleSpecFromSelectionProjectionV3Impl` 的 `makeEdge` 已从计划
依赖复制两端 step/slot，端点 `dependencyId` 经认证投影关联，并用 `findSharedPartialInput`
按消费者恢复具体 SUM step/slot。复用这个 join；仅当新命名或验证边界确实不能携带所需
身份时，在原 projection/edge 补其缺字段。对携带step模板的依赖，缺字段/歧义在入队/
预取前拒绝；普通非step PP保留其合法空step/slot路径，不伪造远程step或空artifact。
这些target/dependency字段用于关联与接纳，不直接塞入共享output的`<RepresentationId>`。TP单步层区间从
adapter循环中已验证的layer/entry事实补入原step模板/codec，PP沿用角色层区间；不能
用unit的全模型区间冒充单步，也不能解析stepId字符串猜层号。

**Epoch/phase投影**：保留NativePlanProjectionBuilder的sequence*stride派生和原packed
operationIndex用于授权/查表；N5 Round只显示producerEpoch。TOKEN_FEEDBACK由producer
epoch e发给consumer e+1，target lookup仍用e+1，Name Round为e。阶段仅按该已知epoch
投影prefill/continuation/execute；提前停止后的CHECKPOINT_FINALIZE仍由已认证lineage
决定，不能为预取假定它总在maxTokens处。step/operationOrdinal完整进入commitment，
不因去掉旧INDEX路径字段而遗失唯一性；operation字符串沿原schema，无别名转换。

两端先从同一源字段计算 producer-output commitment，再派生 Name，最后密封完整 plan。
`NativeExecutionPlanJson` 的 jsonValue/parser/digest、projection/sealer、edge copy 和
`validateTensorReference` 同步调整；保留严格的字段/Producer 比较，不再硬编码旧的
18-component suffix。不能在 source facts 中偷偷塞入含最终 `<RepresentationId>` 的整个 candidate/plan JSON。

runtime nonce保留ProviderGroupCoordinator::deriveNonce的既有确定性派生，依赖冻结的
capability/manifest、exact Data Name和segmentNo；不是改成随机生成。普通protected
publisher与StoredModel仍沿原随机nonce。实际nonce均不反向加入`<RepresentationId>`，无需新KDF。
保护 generation 在预取前必须确定；未满足时停在原准备/授权边界，不猜测 key epoch。

执行链：projection/角色接纳 → `NdnsfCollaborationDependencyIo` 建立现有有界 prefetch
→ Producer 计算结束后编码/保护/signing → `publishOwnedExactData` 批量冻结原包
→ consumer 获取并验证 manifest、framing、签名/保护/执行绑定 → 解包并交既有 runner。
N5 的 inline-first 在 MANIFEST Data 内携带 segment 0，不生成第二份原包；允许的
pre-publication framing 选择保留，发布后禁止同名 fallback。检验实际 Data 包数与逻辑
segment 数，不能把省掉的 segment 0 Data 算成丢包。

`endpointDigest` 仍可承诺完整端点和固定获取约束，但不直接决定output `<RepresentationId>`。每个output
由现有publication owner为每个**output representation**冻结一个publisher endpoint、
capability/group operation、transport manifest、framing及原包快照。仅当consumer引用
同一保护快照时共享原wire；per-consumer target slot在各自密封edge验证，不重复seal。
不同保护快照可封装同一个逻辑output，各有R，不能跨组共享密文或扩大授权。允许consumer集合和实际target
layout属于保护/表示约束；不得把新增授权成员或重布局误称为“只改路由”。

本Spec的策略不变性只指该快照内的**本地fetch策略**：timeout、cwnd、prefetch estimate
和locator只改变现有retrieval reference/operation的局部状态，不重写edge的已密封字段，
不更新publisher的 `bundle.expectedBytes/expectedSegments`，不重建GroupCapability或
CollectiveOperationManifest。effective timeout/window取本地设置与已接纳硬界的交集。
序列化的硬预算、group membership、capability/epoch/protection或full plan事实变更，
必须走原重接纳流程并分配新reservation/表示；此时不要求相同`<RepresentationId>`或相同wire。

这保留 `NdnsfCollectiveControl` 的plan/deadline AAD、`ProviderGroupCoordinator` 的
capability/manifest KDF及nonce上下文；不重写密码协议。publication fingerprint也保留
冻结的密码/内容事实，但不得从consumer的可变fetch状态重算；查已存在的object时，先
验证当前edge有权引用该冻结快照，再逐字节重发原wire，不构造新timestamp/nonce/signature。
新`<RepresentationId>`构造只读N3源字段；冻结后的capability/manifest作为当前接纳/保护证据，不能回流使`<RepresentationId>`
与final plan形成循环。Producer/callback若试图同`<RepresentationId>`绑定另一个保护快照，拒绝冲突。

复用 Spec252 的 shared-partial 规划、认证 capability 和路径。`NativeExecutionPlan` 按
生产 slot 合并符合条件的 reader，接收 stage 在本地 `SUM`。`NativePlanSealer` 检查已选
Provider 能力。`NativePlanProjectionBuilder::certifyEndpoint` 绑定完整受众，并排除 reader
特有的 `consumer_role`、`dependency_id`。因此该端点在已接纳受众内可共享原 Data。
独立保护/兼容路径仍沿 `NativeGroupProjectionBuilder::transferKey` 分配 operation，不能
把 per-edge ordinal 当作 tag8 源语义。新的预留计算绑定原操作及保护事实，复用原
KDF、reader 权限和 step/slot 接纳。测试同时覆盖同受众取同名同 wire、不同 Producer
的两个 partial、错误 reader/slot 拒绝、隔离的保护 operation、能力不足回到兼容计划。

实际大小由最终 manifest 决定；
合法 estimate 差异调整窗口，超硬界拒绝并取消。失败/取消排空既有 handles/timers，
保持现有 byte/inflight 上限及终态 fencing，不创建新的 retry loop。

输入走 `ServiceUser::makeRequestScopedInputDataName` → Selection/RequestSecurityBinding
→ Provider 现有重建/解封路径；新 InputPrefix 在加密前共享并成为 AAD。小输入为 exact
单包，可 inline 原 wire；大输入保留 typed v=0/seg。不发布第二份 APPLICATION_INPUT。

事件走 `InvocationStream::makeInvocationEventName` → `StreamEventPublisher` → Core
stream consumer。在原StreamRequestOptions/StreamBinding中携带通用语义坐标
`publicRequestId/serviceLabel/streamLabel`，与完整Core RequestName、service、attempt、
generation/streamEpoch一起进入codec/canonicalBytes和认证绑定；User发送和Provider
重建两端同步。新DI事件按这些已接纳坐标重建Name；缺少应用坐标的普通Core调用保持
原event规则，不引入isQwen/isDI特判或Name查询服务。签名/AEAD、exact parser/replay key
与End/Response/final-result仍绑定完整Core身份，不能用可读label替代授权事实。

## C5. KV Checkpoint and Retained Core Carriers

KV 使用 `nativeProtectPortableKvCheckpoint` 的**现有 header-AAD 加密**，随后
`protected-kv-packets.cpp` 合并 encrypted-wire 与 packet descriptor 事实生成一个 `<RepresentationId>`。
更新 certificate-aware packet root/segment helpers、reference codec、packet root recomputation、
recipient capsule 中的 object reference，以及 `native-kv-publication-repo` /
`native-kv-restore-repo` 的 grant/restore 校验。把 N5 的 conversation/epoch/role/layout
从当前 checkpoint export/receipt 传入并验证，不能从旧 hash 逆推或在 header 加密后改事实。

`NativeProtectedKvCheckpointReferenceV1::objectName()`没有packet signing certificate，
其Owner加密证书也不是Provider的packet签名证书；它只保留底层encrypted-byte/header/key
reference身份，不让它计算新`<CheckpointPrefix>`。certificate-aware root/prepare helper新增一个具名
`NativeKvPacketNamingContext`参数（conversationId、epoch、roleId、stateLayoutLabel），
由已有认证export/receipt提供并核对checkpointIdentityDigest；不是caller随意指定名称。
由原packetization owner把已有export/receipt中的
conversation/epoch/role/layout和签名证书传给certificate-aware packet root helper，计算
并验证`<CheckpointPrefix>`，返回并保存 `rootName=<CheckpointPrefix>/MANIFEST` 于原
`NativeProtectedKvPacketSet.rootName` 和saved packet reference；
`<CheckpointPrefix>=rootName.getPrefix(-1)`，不另存一份可漂移prefix。storage grant的dataPrefix取`<CheckpointPrefix>`，替换
当前 `native-kv-publication-repo.cpp:100` 的byte-reference.objectName调用；restore从保存
的exact root、签名证书和已认证checkpoint上下文重算`<CheckpointPrefix>`。计算`<RepresentationId>`的packet descriptor不含最终`<CheckpointPrefix>`/rootName/签名值，避免packet名自引用。
这是新packet发布的grant规则；旧saved packet/grant按其原记录验证，不把原dataPrefix
改成新K后宣称复用。新旧两条闭集读法都验证真实root、Producer及对应授权范围。
两张证书和两层identity均验证，
不把旧byte reference冒充真正发布的packet Name。旧packet descriptor按旧闭集记录读取。

不将最终 `<CheckpointPrefix>`/`<RepresentationId>` 放回已完成 KV AEAD，不重做 KV 内部页命名；所有原签名、certificate pin、
FinalBlock、状态/epoch/current recipient 校验保留。新 Provider 只恢复原包，下一次自己
产生的 checkpoint/activation 才有自己的 Producer。原 handle 的 wire/name 读取不变。

N6 的 Core 四消息、COLLAB、RPC、security profile、key grant、deployment、Stream 外层、
证书/NAC/SVS/NDNSD 不做审美改名。原包冻结与终态冲突由现有 owner 负责：先查对应实现/
回归，再以生产 fixture 证明重试不会同名换 wire。若有真实缺口，在原 publish/cache/
retry owner 最小修补，不创建 Core 新命名版本或第二条 `<RepresentationId>` 发现协议。
必须更换 Request/ACK/Response 字节的调用结束旧 `<RequestName>` 后，由当前 requester
分配新 `<RequestName>`；上层会话关联保留，不新增单独的公开映射服务。

## C6. Build, Native Evidence and Completion

复用现有 Waf libraries，生产定义落在 Core naming/manifest TU 与 DI projection/publisher/
dependency/KV TU。新增 fixture 注册 `tests/wscript` 的现有生产闭包；不增加另一套 build
目录、复制二进制或 Python 行为实现。薄 binding 的字段转换和 raw Name wire parity 同步。

| Scope | Existing targets / fixture owners to extend |
| --- | --- |
| components/model publication | `naming-codec`、`naming-static-publication`、`naming-original-packet-publication`；producer-data-naming/published-content-name fixtures |
| canonical/fixed/capsule | `model-contracts`、`di-canonical-publisher`；native model contract/publisher fixtures |
| StoredModel | `stored-model-capability` 及原 Repo 重启读路径 |
| tensor/projection/prefetch | `naming-collective-names`、`di-epoch-projection`、`spec242-hybrid-cut-tests` 及 `exact-tensor-transport` 的生产 projection/IO fixtures |
| events/Core | `naming-event-conversation`、`naming-invocation-names`、`exact-data-wire` |
| KV/Repo | `activation-conversion` 内 native-protected-kv-packets 等 fixtures；`naming-transparent-repo` |

名称/serializer独立 oracle 不调用被测 builder 来生成 expected Name；手工 TLV 向量核对
字段集合、tag/kind/顺序、全长身份。行为 oracle 使用真实 C++ publisher/reader，native
断言不放 Python。Waf source closure、fixture 生命周期和失败清理属于每项原验证范围。
按 Spec253 的 fixture 边界准备微型 ONNX/权重和固定预期，默认路径不读宿主模型缓存；
T005 真实 INT4 由显式资格入口接纳已准备资产。测试 manifest 的字段只负责资格检查。
若已有 target 不包含新 fixture 的必要 production TU，在原目标补闭包或注册一个有明确
缺口的新目标，不能先猜链接成功。实际验证命令以最终 Waf 注册为准并记录。

最终安装后复用 `ndnsf-di cluster up --config <DeploymentConfig>` 的 MiniNDN launcher/
native oracle，限定两 Provider、本地现有 Qwen3-0.6B INT4 CPU、32-token cold/warm：
cold 输入25生成32，warm追加24，起始context81；核对模型/config SHA 和独立输出/KV。
运行前检查实有模型/基线，缺失记 gap，不下载替代或静默换量化。使用原 cold/warm 和
nonforced cleanup，数据按原 evidence 规则归档，汇总后清理本任务 scratch。

记录 wall、ONNX、分阶段 payload/wire bytes、Name bytes/包数及已有 delivery 指标；
没有同候选配对证据不宣称性能提升。激活时已有完整 TP 则复用其最短生产回归；未实现的
TP 输出路径保留未资格状态，以密封/IO 原生边界验证命名，不借此扩大 Spec 225 到 TP 实现。

动态验证复用已有 ASan/UBSan 工具链，针对改动 codec/IO 的正常、损坏、超长、取消与重放
等价类冻结一张有界卡；不新增完整 fuzz/TSan 工程。纯文档本轮为 none/NOT_RUN。正式
MiniNDN 前再做代码感知审查，发现控制性 gap 留在对应任务修正，不能凭静态审查宣称通过。
