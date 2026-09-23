# Stage Transfer and Persistent Material Reuse

**Status**: proposed；本契约扩展CD-01–05，不表示当前protected路径已有热缓存能力。
适用G1–G6、C1–C3、D1–D4、R1–R6与B/M约束；不引入新加密算法或通用Repo重构。

## Cache State Matrix

| State | Persistent Repo query/store | Provider material network | Assembly | ORT load | KV |
| --- | --- | --- | --- | --- | --- |
| cold | query miss→一次有界提交 | Selection后仅所分配缺失材料 | yes | yes | 新建 |
| layer disk hit | query+verified hit，无STORE | 0 payload，允许小型授权/manifest | 可需要 | yes | 独立校验 |
| assembled disk hit | query+verified hit，无STORE | 0 payload | no | yes | 独立校验 |
| resident hit | 同上 | 0 payload | no | no | 当前有效父KV才复用 |
| process/Repo restart | 相同根恢复，合法hit无STORE | 完整磁盘材料hit为0；miss精确补取 | 依磁盘状态 | 必须新load | 新boot不得接受旧本地KV receipt |
| wrong digest/key/policy | 不记hit；当前验证拒绝或新identity | 仅合法缺失/新版本材料 | 明确报告 | 依状态 | 不沿用失效状态 |

零材料网络是可验证目标，不是当前r260事实。缓存查询、重新授权、摘要、receipt、Interest和控制仍可能有流量。
层缓存不能免除组装，组装缓存不能免除磁盘到内存/ORT初始化；session常驻才免除这部分。
重启场景重新开始有效对话或按现有合法恢复路径重算，不把磁盘模型热启动等同续用旧KV。

## CD-06 Stage Transfer Budget

**Existing path**：`ProviderRoleWorker::outputForEdge/withoutProviderLocalState`→
`NdnsfCollaborationDependencyIo::publishOutput/prefetchInput`→Core。
`NativeEpochCoordinator::lastLogits/makeTokenFeedback`本地采样再发token/stop，不应发送完整logits。
**FN delta**：公开业务签名不改，传输owner增加按request/attempt/edge/epoch关联的观察记录；
T001日志基础复用。不根据`NativeRuntimeMetrics.activationOutputBytes`直接推断wire量，它可能包含本地KV。
**Budget**：实际发送tensor集合S的纯数据字节为 `sum(dtype_width(t) * product(actual_shape(t)))`。
hidden形状`[B,q,H]`时为`B*q*H*w`；q分别为首次prompt长度、续轮delta长度、decode的1。
若确实发送Int64 mask`[1,L]`/position`[1,q]`，再计8L/8q；不能假设这些字段一定存在/不随前缀变。
feedback tensor含Int64 token和Bool，编码/lineage/manifest/加密开销另计。finalize单列，不算新增token。
**Counters**：unique tensor bytes、encoded bundle bytes、material bytes、ciphertext Data wire、Interest、retry、
metadata/control、local copy bytes；标send/receive/测点，不能把两端相加当同一链路净流量。
重传同时进入wire totals和独立retry计数；累计counter取相邻snapshot差值，不重复累计。
**Repair policy**：先量production对象再修多发/重复物化。mask/position仅在现有认证lineage可等价重建、
sealed edge contract/发送和接收双方同时改动且负例通过时才删；不静默省字段，不改数值精度。
本地KV/logits/分段copy若在关键路径占显著时间，在原owner内限定修复；不自动重写整个tensor/runtime框架。
**Proof**：扩展`NativeEpochCoordinatorKeepsDecodeStateProviderLocal`和真实dependency publisher fixture，
动态prompt/delta/decode/finalize，对实际序列化结果独立解码计数；注入额外KV/权重/logits应失败。
数值输出、lineage、cancel/retry仍正确；仅包计数或loopback空消息不算通过。

## CD-07 Stable Per-node Repo Owner

**Existing API**：`makeFilesystemRepoStore(rootPath,maxRangeBytes,vectorCompatibilityThreshold,ownerId)`、
`RepoCore::has/getManifest/getRange/putRange/commitRanges`、`RepoNode::registerServices(ServiceProvider&)`原样复用。
后端已有digest payload、原子manifest、`recoverOrphans()`及`BackendOwnershipLease`排他flock；不新增数据库系统。
**Configuration**：显式`repo_persistent_root`与稳定`deployment_id/node_id`，映射
`<repo_persistent_root>/<deployment_id>/<node_id>/`；内部canonical/material/ciphertext子区固定，
对象digest可分层，不能含runId/PID/时间戳，也不能把所有节点指向同一后端writer。
根必须在launcher reset/cleanup范围之外；默认不迁移/删除历史目录，仅新profile映射使用已核对固定根。
LocalExperiment 的 `prepare --cache-dir` 只指定该稳定缓存根的宿主路径，并通过 launch record
传给 native runner；它不是每个run的Repo根，也不能落在run evidence/workload目录内。source
Repo和Provider assembled cache仍按完整identity/recipe digest分区，不能因目录相同而把请求态、授权态、
加密Repo、日志或旧KV写入共享缓存；默认的普通protected路径仍必须走当前请求的授权和Selection。
**Ownership**：每节点一个C++Repo owner，多个本地caller共享owner，远程通过已安装RepoNode/Core服务；
不得每个requester各自打开同一root。并发第二owner明确BUSY，不夺锁/清空目录。
可在所属C++应用内持有Repo，不强制引入新daemon；跨进程共享时接入现有服务入口。
in-app仅应用写入/主动fetch缓存；需要User远程提交的节点使用server模式并保留准入，不能绕过R5。
**Restart**：加载manifest/catalog元数据、校验完整提交和范围索引，恢复服务owner/路由后声明ready；
不把整个payload库读成内存vector。catalog epoch若进程重置，带boot/source版本重新取snapshot，
旧增量或历史locator不能作当前可达性证明。读数据仍校验digest，不能只相信sidecar里的hash文本。
**Cleanup/budget**：退出释放owner/lock，committed材料保留；run cleanup只能撤销本次owned staging。
固定根path/symlink逃逸校验、目录权限、owner锁、磁盘配额/预留、孤儿恢复、引用/active-read保护必须测试。
正常新run不自动GC有效payload；满额时有界拒绝，不能删共享材料或改成新run目录。测试用独立临时根，RAII收尾。

## CD-08 Query-before-store and Stable Publication Identity

**Existing path**：`RepoSourceProvider::load/publish`已经manifest-first、root-last并有receipt hit校验；
`NativePreparedCanonicalPublication::rollbackOwned=false`保护已提交数据。先复用/接线，不再造另一份publisher。
当前根`artifactRoot/prepared/<sourceDigest>`不足区分不同initializer/profile/layer组合；
改为完整publication identity命名，冲突创建新identity，payload仍按自身digest去重，不同run不生成新identity。
identity绑定source/graph/initializer/material-manifest/adapter/profile/layer集合、service/security domain、
必要policy/key epoch；request/attempt/runId不是immutable内容身份。
**Query flow**：User/Runtime prepare先读取小manifest/receipt，校验身份、提交状态、依赖闭包及当前访问权限。
`user.prepare(model)`是此缓存的公开受益入口，不是先完整prepare再只去重STORE。
查询identity应在拆层/导出/打包前由已验证源身份及准备配置确定；不能依赖先重新生成全部产物才得到查询key。
区分prepare lookup key（源内容digest、准备配置/算法版本、service/security domain）与receipt中的产物identity。
后者的graph/material-manifest/output digest由已提交receipt提供并校验，不要求调用方先生成产物才能构造lookup key；
不可仅按文件名/mtime认定源未变，可信不可变源reference或实际内容校验成本独立记录。
完整命中返回与冷准备等价的PreparedModel材料/IO契约和当前有效owner；不得直接反序列化旧进程handle/grant。
允许必要有界校验读盘，记录hash/inspection耗时；不得因此重跑split/export/package。
本地走RepoCore/RepoClient；远端走`RepoClient::requestManifest`及现有授权回调，不用Python扫目录代替网络查询。
manifest hit且全部子对象可用→复用reference/receipt，不再STORE/ingest/复制/重新拆层。
缺对象→只对缺失对象/范围执行现有事务提交；坏digest/冲突不能伪装普通命中或静默覆盖好对象。
不能因为校验读盘就统计成material network；校验成本单列。避免hit路径仍将整initializer复制进vector。
**FN delta**：保留`RepositorySourceProvider::load(request,fallback)`和`RepositoryArtifactPublisher::publish(...)`；
增加可选native `lookupPrepared(const RepositoryPublicationQuery&, const NativeRequestControl&) const`
返回`std::optional<NativePreparedCanonicalPublication>`，默认nullopt保持其他publisher兼容。
query为由当前验证catalog/manifest产生的小型身份，不接收未认证的任意path；具体字段按上述identity canonical编码。
Runtime在大对象读取/重发布之前使用lookup；hit仍验证model/IO/当前授权，不能跳过必要graph inspection。
miss仍走原source load/publish路径。完整receipt与serving owner由同一publication owner提供，不持久化raw指针。
**Preparation owner**：lookup接入`ModelPreparationCache`，位于`spec.loadSource`与`NativeRequestCatalog::load`前；
仅在Runtime/publisher加去重不足以跳过当前先load/catalog后publish的路径。
新增版本化`PreparedMetadataV1`：lookupKey、source/configuration/catalog-configuration/task-contract/input-layout digests、
adapter ID/version、manifest、capabilities、graph/IO/layer及initializer引用闭包、publication receipt/其digest。
元数据必须root-last提交、大小有界，校验schema/全部身份、当前adapter支持和引用完整性；未知版本/不支持恢复为显式miss，
篡改/无权限为错误，不静默当hit。manifest与capabilities不得仅相信未验证旧JSON。
由当前native catalog factory从已校验graph/IO引用重建catalog及preparation/splitter adapter，复用已有注册表；
轻量重建FrozenPreparationRegistration、默认placement和当前runtimeBinding，绝不恢复旧指针、grant或cached plan。
不支持reference-only恢复的adapter保持明确MISS并不得计为Qwen热路径通过；T008必须补齐Qwen native factory恢复支持。
新建PreparedModelPackage/wrapper及当前serving lease是必要轻量重绑定，不计为重复材料打包。
所有计数绑定实际production材料生成/序列化/提交调用点；冷路径必须展示对应非零计数，
不存在split/export独立调用时记录真实等价生成入口，禁止对从未调用的占位counter断言0制造PASS。
**Race**：lookup→publish的TOCTOU由现有publication lock+transaction operationId/commit-if-owned关闭；
另一调用先提交相同完整identity则重查复用，不删除对方结果；错identity保留两者或拒绝，不覆盖。
**Proof**：真实backend close/reopen、首写后第二次STORE计数0、缺一子对象、截断数据、相同source不同profile、
并发同identity、满盘/中断/取消/半提交恢复、旧run cleanup不得删除已有receipt材料。
同进程第二次及新进程重启后再次调用production `user.prepare(model)`：split/export/package/STORE调用计数均为0，
返回契约与冷准备一致；改变模型内容或准备配置不允许错误命中，缺一对象只修复该对象的依赖闭包。

## CD-09 Protected Material Reuse Boundary

**Confirmed gap**：普通protected路径目前在`tryLoadNativeCanonicalOnnxRoleFromCache`强制miss；
不能直接删除该检查。`RepoEncryptedLargeDataStore::Source`析构会removeIfCurrent，
Core发布名含request/version，保留目录并不能恢复密钥、receipt与网络serving。
**Target contract**：canonical内容与受保护publication分别建identity。受保护identity必须绑定实际
publisher/trust domain、ciphertext digest/完整manifest、加密算法/AAD契约、真实key identity/version、
policy epoch与可用key-reference；`epoch-1`字面字符串或相同plaintext SHA不足以命中。
Repo透明保存bytes，不获得或生成解密密钥；Core/既有授权owner管理密钥引用与当前grant绑定，
新请求仍grant→Selection→placement验证，只有授权后可用本地材料/assembled缓存。
密文与key-reference均合法且serving已恢复→复用原不可变对象，当前请求使用新的授权/绑定，不重复写大payload。
key丢失/撤销/改变或trust不匹配→不命中，精确报告原因，按现有合法路径发布新身份；旧对象不覆盖，预算受控。
Provider缓存只持有按policy可保留的材料及可验证身份；不永久留存原本只获request-scoped明文lease的数据。
若既有policy禁止跨请求保留，保持MISS并明确本场景未达hot-path验收，不将cache-compatibility旁路算修复。
**Persistence ownership**：request-scoped transient仍沿用当前析构清理；新增durable material显式owner/retention，
退出仅释放活跃lease，GC仅在明确失效且无活跃read时执行。key-reference恢复失败不能将有ciphertext文件称为可用Repo hit。
稳定producer身份/证书或合法信任迁移、locator/路由恢复均要真实验证；Provider boot变更使旧KV失效。
**Design gate**：T006先冻结Core crypto-owner提供的durable key-reference/serving恢复接口、
加密identity与新grant重绑定、缓存retention policy及错误/取消契约，然后才改公共API或默认protected路径。
当前仅有目标边界，不能写成此项生产编码READY；到T006时先闭合安全设计门，未闭合则停在T006，不能开始T007。
不用存储整份旧request/grant/会话key JSON来凑重启复用，不换明文传输绕开问题。

### T006 Target interface freeze (not current behavior)

以下是实现前冻结的最小公共契约；它描述目标接口，不表示当前源码已经提供这些能力：

- `EncryptedLargeDataRetention` 只有 `Transient` 与 `Durable`。现有无选项的
  `commitFile(name, file, size, requireActive)` 保持 `Transient` 语义；任何实现收到
  `Durable` 但没有持久 owner 时必须返回明确的 `DURABLE_RETENTION_UNSUPPORTED`，不能静默降级。
- 新的 `EncryptedLargeDataCommitOptions` 只携带已由 Core 校验的 publication identity、
  `protectionEpoch`、opaque `keyReferenceId`/version、ciphertext/encryption-manifest digest
  和 serving locator。它不携带 plaintext、private key、旧 grant、完整 request 或会话 KV。
- `EncryptedLargeDataRangeStore::commitFile(..., options, requireActive)` 的 owner 只提交并
  校验 ciphertext bytes；`EncryptedLargeDataRangeSource` 的 durable lease 析构只释放读取
  lease，不删除 committed object。删除只能由 Core 的显式 invalidation/GC 操作执行，并且
  必须检查 identity、无 active read 和当前失效状态。
- `ServiceUser` 是 key-reference owner：冷发布时创建或恢复 opaque reference；重启后先校验
  identity、key version、policy epoch 和 reference digest，再重新注册合法 serving；随后每个新
  request 仍必须独立完成 grant verification、Selection 和 placement 检查。Repo 不生成、不解包、
  不接受 grant，也不决定授权。
- `LargeDataPublishResult`/prepared receipt 必须同时记录 ciphertext manifest 与 key-reference
  identity，且 rollback 只撤销本次 owned staging。新请求的 `requestId`、`attempt`、Provider
  `bootId` 和旧 grant 不能成为 durable identity，也不能被恢复为旧 session/KV。

只要其中任一 owner、reference、serving 或新 grant 绑定失败，结果就是 typed miss/rejection，
而不是把“Repo 里还有文件”报告成 protected hit。实现必须先以 C++ 反例验证这组不变量，之后才可
解除 `tryLoadNativeCanonicalOnnxRoleFromCache` 的 protected miss 门。
**Proof**：真实protected冷发布→新run同identity查询hit→Repo重启恢复服务→当前新grant命中本地层/assembled；
旧/错grant、失效key、篡改ciphertext、错AAD、错boot/KV、取消与活跃lease等反例仍拒绝；
缺一层需走真实选后Repo fetch，其余层payload为0，whole-model/无关role fetch计数为0。

## Validation Profiles

1. 既有cache-compatible profile仅作历史/算法诊断，不满足新增Repo场景。
2. Repo-enabled cold：固定根第一次提交材料，Provider实际按分配fetch/assembly/execute，保存C++材料oracle证据。
3. Repo-enabled warm：同根、当前新授权，完整材料hit免重复STORE/网络payload；assembled与resident分别计数。
4. Repo restart + Provider restart：真实销毁进程/owner并复用同根，新boot/newsession，起新合法对话；验证物理payload不增长。
5. Missing/corrupt material：受控C++小模型fixture及单次真实selected cache miss，证明不是共享路径绕过fetch。

三组性能配对在相同Repo-enabled warm状态比较ACK/应用/驻留策略，不能一侧cold/一侧warm冒充纯ACK收益。
首次冷入库与三次重启是独立功能/磁盘验收，不为每次run复制一个大Repo控制目录。
报告prepare-query、hash校验、store/ingest、encrypt、material fetch、assembly、ORT load/warmup、run/stream各阶段；
“快速进入推理”由哪些阶段实际跳过与剩余耗时共同证明，不承诺layer disk hit能消除load。
