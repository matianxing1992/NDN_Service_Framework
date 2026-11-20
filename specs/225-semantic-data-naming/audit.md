# Design Audit: Spec 225

**Status**: DOCUMENT_REVIEW / NOT_ACTIVE
**Progress Timestamp**: 2026-11-20 00:31 -06:00
**Pinned baseline**: `spec253@cd9246fdc18007b32e45f0c742a9ed98f33f8976`
**Scope**: Spec225 的需求、计划、任务与合同；旧审计基线为 `96230e82ef525908be81838e2699cb9aab6ade4e`。
**Excluded**: 产品代码、现行活动 Spec253 和构建/实验。

## Spec253 Re-audit

通过本地提交读取 immutable source：`git diff --stat 26b9ebf3..cd9246fd` 确认 Spec253
在 Experimental 之上的提交集中于测试、安装、文档和 `OperationRuntime` 队列管理。
HEAD `cd9246fd` 的改动是显式 INT8 用例的资产缺失提示。进一步对照原 Spec225 基线到
当前提交，重核已合入的制品准备、Repo 以及 Spec252 张量路径。结果是下列静态审查；
命名目录仍是拟议语法。

| 现行事实与遗漏 | 固定源码依据 | 修订与依赖 |
| --- | --- | --- |
| PP、单 stage TP、hybrid、完整图和图分区有实际计划或本地组装入口，原目录只用整图、层区间和 TP 三种坐标 | `NativeRequestCatalog.cpp:469–705`；`NativeQwenPlanner.cpp:114–192`；`automatic-onnx-split.cpp:265–325,404–488`；`NativeYoloPlanner.cpp:47–126` | N4 展开各切分方式及 entry；C1 沿原 adapter/recipe 投影；T001 |
| degree/rank 相同可有不同加权 KV/FFN 切片，角色放置 degree 也可能为 1 | `native-qwen-tensor-split.cpp:219–286,3731–3766`；`NativePlanning.cpp:92–129`；`NativeCanonicalRolePreparer.cpp:346–488` | N4 显示实际范围，完整 shard/配方仍受认证；错范围/entry在原接纳边界拒绝；T001 |
| 线上实际发布源图或共享材料 bundle，碎片由 Provider 本地组装。旧 `ArtifactFile` 只有单组件坐标；有指向本地对象的 Name 型 alias | `NativeCanonicalArtifactPublisher.cpp:174–227,1378–1437,1754–1768`；`NativeCanonicalOnnxAssembler.cpp:3735–3848`；`PublishedContentName.cpp:67–84,129–133`；`NativeRequestPlanner.cpp:2365–2381` | C1/N4/N6 保留原缓存、材料和 exact 引用；在现有组装记录提供可读坐标，独立发布沿既有 API；T001 |
| StoredModel 加密业务根、索引、外层 root 各有身份。Repo range/commit/索引操作走既有管理路径 | `stored-model-preparation.cpp:116–122,161,212–215`；`ArtifactRepositoryApi.cpp:2915–2947`；`RepoNode.cpp:642–698,1790–1827` | N4/C3/N6 记录真实对象 ID 和存取边界；现有 handle 复用并精确读回原 wire；T002/T004 |
| 原来描述的 step/slot 缺口已部分闭合。Spec252 绑定完整受众，复用来自同一源的原包；接收 stage 的 SUM 保持本地 | `NativeExecutionPlanJson.cpp:4429–4540`；`NativeExecutionPlan.cpp:691–777`；`NativePlanSealer.cpp:714–740`；`NativePlanProjectionBuilder.cpp:263–280` | N5/C4/T003 删除重复桥接要求，维持认证关联、真实 exact IO、能力回退和原密码边界 |
| 驻留/链路观测在 ACK V4，测试用 asset manifest、fixture 与原生调度的 queue bookkeeping 属各自现有边界 | `NativeProviderOfferV4.cpp:1057–1095,1323–1328`；`NativeOfferAdmission.cpp:343,366–367`；`tests/run-checks.py:544–573` | N5/N6 说明嵌入和本地用途；按 Spec253 组织原 native 证据，不新增应用 Data |

本轮复用 T001 的制品与 codec 入口、T002 的持久读回、T003 的共享 partial 和 T004 的 KV
路径。分别补缺，不增批次。真实网络发布、同一已获准受众内的 exact 原包复用、旧 record
兼容与现有能力不足回退，都仍由相应原生验收承担。

## Original Scope and Batch Consolidation

核心成果是producer-owned、可解释、不可变并能预取/复用的Data名称，不是重做Core协议、
TP运行时或Repo。用户明确要求暂不激活；本轮创建一个待执行Spec，不改feature pointer。

`Batch growth decision = design consolidation only`：五个连贯实现/验证单元已经覆盖必要
行为。没有新增GPU、27B、fuzz平台、在线alias服务或通用版本兼容任务。名称目录只维护
一份；旧设计页改为链接入口。本文不颁发STATIC_PASS/BUILD_PASS/DONE。

## Original Findings and Corrections

| Finding / consequence | Disposition | Correction and binding |
| --- | --- | --- |
| 单字母角色/对象缩写难读，且项目未发布 | keep user correction | 全部描述性占位符；不加发布版本路径/协商，保留NDN typed version、模型revision和body schema；N2 |
| 用未来ciphertext或最终plan digest推导`<RepresentationId>`会阻塞预取或自引用 | correct | 先source-side事实/reservation→`<RepresentationId>`→最终密封，真实内容由manifest绑定；N3/C2/C4 |
| 整个endpoint摘要含target slot/获取策略，fanout或改窗口会重命名output | correct | producer-output identity与edge admission分开，完整plan仍验证；C4 |
| 仅改Name摘要仍会让AAD/KDF/原包fingerprint随策略变化 | correct / simplify | 把不变性限定为冻结publisher/capability/manifest快照内的本地fetch策略；保留密码绑定和原wire，改变硬授权事实则新接纳/新表示；N3/C4/T003，不重写KDF |
| 要求实际segment count提前已知会把estimate当事实 | correct | 已知geometry和最终count区分；合法estimate差异调窗口，超硬界失败；N3/N5 |
| 普通Data目录漏掉inline-first的实际包数 | correct | MANIFEST Data携带segment0，省掉独立DATA/seg0；只允许发布前一次framing选择；N5/C4 |
| 原proposal暗示加密nonce也必须由User预分配/新KDF派生 | simplify | 只冻结发布reservation/nonce policy；实际cipher nonce沿原publisher生成一次并保存，不加入预取`<RepresentationId>`；C2/C4 |
| 仅换prefix会被category allowlist或ProtectedBlobManifest精确检查拒绝 | correct | T001登记新实际类别，T001同时改stem/保护/manifest/signer和reader；C1/C2 |
| StoredModel名参与key/AAD，object/index读法也受分段prefix影响 | correct | journal/prefix/key/seal/index/root/read一起迁移；新路径用exact segment API，旧handle读原包；C3 |
| KV先header-AAD加密，不能把最终packet名放回AEAD | reuse | 保留现有seal-first，随后计算完整wire/packet descriptor承诺；C5 |
| 为每种业务JSON、Core控制、Repo操作或local SUM新增Data | remove | 沿原COLLAB/RPC/Response；API-only/embedded/local/external分类完整列出，不造publisher；N4–N6 |
| 只见ACK/Response名无attempt，就预设全Core必须重构 | remove | 保留既有语义名，先用生产回归查原包冻结/终态；有确证缺口才原owner修补；C5 |
| 历史inventory引用的Python page publisher已退役 | correct | pages只标API/schema边界，不把retired入口算作active publisher；N4与下表证据 |
| 将所有已有包改名，破坏跨实验复用 | remove | 实际仍需复用的exact handle继续读原包，不建通用版本层、不强制重发布；C3/C5 |
| 命名任务暗中依赖未完成TP或远端GPU资源 | defer | PP安装CPU验收和已有TP命名/IO边界；不接管Spec224 runner/session/GPU工作；C6 |
| T001“全部N4对象”字面包含后续StoredModel专用builder | correct | T001只验普通PublishedContentName对象；package/object/index/root由T002自己闭合，避免验收反向依赖 |

## Historical Source Coverage

下表保留初版 `96230e82` 的证据和行号。与现行分支的差异以本页 Spec253 复审为准。

初版在 `96230e82` 沿完整链核对的接收和恢复边界如下：

| Breakpoint and source | Required correction / independent observation |
| --- | --- |
| ProtectedBlobManifest:136–140；ServiceUser:8071–8077旧canonical重建仍调contentPrefix | C1/C2保留旧canonical算法与闭集记录dispatch；新旧durable比较完整事实并返回原Name，不把AllowLegacyRead当通用兼容；T001验证真实持久读回 |
| CanonicalArtifactPublisher:209–226旧manifestSuffix override | 新发布只让Core生成stem/rep/MANIFEST，suffix为空；旧override只读原记录；T001验证root/material/receipt引用与wire |
| model-object-keys:631–633仍调fixed-source validator；ExecutionPlanJson:3329–3343只认assignment | capsule获取前用正确类别/source/use验证，之后保留完整recipient/boot/expiry/schema；T001走install→object fetch并做跨kind负例 |
| stored-model-capability:943–961先建journal后bindPrefix，488–493默认prefix无ModelPath | C3在同一factory/journal前传semanticPublication、锁内read-or-reserve并持久化；T002分别验default/explicit/restart，不重导模型 |
| stored-model-capability:623/637用新helper重算旧journal entry | 旧record按旧算法验证/恢复原包，ready直接复用，partial只原样恢复、缺数据不修改state地失败；T002真实重开 |
| NativeInferenceClient:522–527、2434–2501区分public/core request与attempt | C4认证绑定完整三元组；transport/AAD/End仍用Core身份，replacement只一次且旧attempt拒绝；T003验证1→2及warm新1 |
| ExecutionPlanJson:1263–1269有dependency tuple，4019–4111却未复制到runtime edge | C4保留dependencyId及两端step/slot/epoch，不能只按role对；作为当前224基线缺口标明，T003只补原适配链，不造scheduler |
| EpochCoordinator:1333–1365、1610–1616、1781–1788采样后才知道早停/finalize | Name只能提前用prefill/continuation/execute，实际FINALIZE在认证lineage；T003验早EOS/stop，不能假设maxTokens才finalize |
| ProjectionBuilder:197–218与DependencyIo:37–48分packed index和feedback两侧epoch | Round明确为producerEpoch，保留原packed index/ordinal做授权查表；首feedback round0消费epoch1，T003对账 |
| Qwen tensor splitter:948与PP/feedback已有schema字符串 | 直接沿用TENSOR_PARTIAL/ACTIVATION/PIPELINE/TOKEN_FEEDBACK，不造operation别名 |
| GroupCoordinator:1906–1937实际派生tensor nonce | C4明确保留现有deriveNonce；普通blob/StoredModel随机nonce与tensor派生nonce分开描述，均不反馈进入R |
| NativeGroupProjectionBuilder:13–18、101–115的operation可源于per-edge ordinal | 同一保护快照才共享原包；不同既有group/operation/KDF表示各有R，ordinal绑定保护事实而非source step语义；T003不新增fanout聚合或扩大capability |
| protected-kv-checkpoint.hpp:byte reference只有Owner证书；packets.cpp:60–77还需packet签名证书 | C5由certificate-aware packet helper加既有checkpoint context计算K；grant prefix取packet root前缀，不取byteRef.objectName；T004验证两张证书、root/grant/restore一致 |

### Task Consolidation

旧六项均未开始，故允许合并后重编号；没有重写历史执行证据：

| Previous task | Current task | Reason |
| --- | --- | --- |
| 原T001 + 原T002 | T001 | 改contentPrefix/manifest却延后真实caller会形成中间不一致；普通codec与生产发布/读取必须同一原子单元 |
| 原T003 | T002 | StoredModel专用factory/journal/index读链，出口不变 |
| 原T004 | T003 | runtime tuple/input/event/attempt与预取，出口补齐当前发现的边界 |
| 原T005 | T004 | KV packet root/grant/restore，出口不变 |
| 原T006 | T005 | 组合候选安装后资格，出口不变 |

本轮没有增加实现任务；旧hash/新语义的必要记录读取也不是通用多版本框架。审计不对当前
TP生成token或新命名运行成功作结论，所有新行为仍待原生验证。

只读查询覆盖以下五lane；“covered”指设计取证范围，不表示实现/运行通过。

| Lane | Coverage / actual query and evidence |
| --- | --- |
| production entry/callers | covered：`rg -n`/`sed`核对canonical publisher→ServiceUser、native plan→DependencyIo、StoredModel prepare→seal/read、KV export→packet/restore。具体hop与baseline行号在plan及C2–C5 |
| implementation and wire | covered：读取`ProducerDataNaming.hpp/.cpp`、`PublishedContentName.hpp/.cpp`、`ProtectedBlobManifest.cpp`、`NativeExecutionPlan.hpp/.cpp`、`NativeExecutionPlanJson.cpp`、`NdnsfCollaborationDependencyIo.cpp`；确认category、`<RepresentationId>`/manifest构造、18组件parser、inline framing。另读GroupCoordinator KDF与NdnsfCollectiveControl AAD；固定ref差异命令见下 |
| test/harness/oracle | covered for design：`rg -n`/`sed tests/wscript`确认C6所列原target与fixture注册；核对SegmentFetcher安装头约束。新增断言/独立TLV及生产IO fixture均planned/NOT_RUN，不以历史target结果冒充覆盖 |
| build/source closure | covered for design：原Core/DI library、binding和tests目标是预期闭包；新增产品符号未实现，definition map与ELF digest为N/A，本轮无构建产物。激活后在对应任务核对真实TU/target及输出SHA |
| migration/evidence | covered：StoredModel key/AAD/journal/root pin、KV header-AAD与原包读取、Core原carrier、退役Python入口、活动Spec不变；实际跨进程读和MiniNDN证据NOT_RUN |

源码定位：

上一轮已执行以下只读差异检查，输出为空；只覆盖列出的入口，不宣称整个分支无变化：

```sh
git diff --stat 0589b314 5e4a7fd5 -- ndn-service-framework/ProducerDataNaming.cpp ndn-service-framework/PublishedContentName.cpp ndn-service-framework/ProtectedBlobManifest.cpp NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.cpp NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.cpp NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.cpp
```

- [ProducerDataNaming.cpp](../../ndn-service-framework/ProducerDataNaming.cpp)、
  [PublishedContentName.hpp](../../ndn-service-framework/PublishedContentName.hpp)、
  [ProtectedBlobManifest.cpp](../../ndn-service-framework/ProtectedBlobManifest.cpp)。
- [NativeExecutionPlan.cpp](../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.cpp)、
  [NdnsfCollaborationDependencyIo.cpp](../../NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp)、
  [stored-model-capability.cpp](../../NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.cpp)、
  [protected-kv-checkpoint.cpp](../../NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-checkpoint.cpp)。
- [tests/wscript](../../tests/wscript)、
  [spec201 inventory](../../Experiments/spec201_naming_inventory.py)、
  [retired Python backend](../../NDNSF-DistributedRepo/pythonWrapper/py_repoclient/network_artifact_backend.py)。

## Necessity and Dependency Review

T001原子闭合普通codec、表示字段与canonical生产发布/接纳；T002保留模型持久复用；
T003闭合精确预取和输入/事件；T004维持KV移交；T005证明安装后真实推理。删任一项会
分别留下对应FR/SC的生产边界未覆盖。普通对象manifest、StoredModel root和KV包格式
不能为了共享一个helper强行合并。

任务与FR/SC映射在plan，仅维护一份。实现任务均有原生行为出口和明确Design binding，
没有只写报告的实现任务。T003不等待T002，T001不等待大模型或StoredModel后续功能；T005不接收
前项自身验证欠账。每项完成后单独commit；本轮没有任务完成，故不产生任务commit。

## Document Checks and Limits

- 已执行 `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints`。
  项目 11/11 入口通过核对，个人安装的 `references/pre-test-static-review.md` 与仓库包摘要不同。
  使用空的临时 `--codex-home` 复核仓库侧得到 `PASS: 11/11 local entrypoints`。
  本轮采用仓库内入口和共享规则。此检查仅涉及技能同步。
- `.specify/extensions.yml` 只有optional agent-context hooks；用户要求不切换，未运行这些hook。
- 名称目录原盘点SHA为0589b314；本轮固定baseline见页首。未来激活时重查新增TP接线，
  不用历史文档断言未来实现完备。
- 产品构建、native行为、动态工具、安装、MiniNDN、Tiger：均 **NOT_RUN**。

两路只读独立审查分别覆盖标准/安全/源码兼容与需求/必要性/任务调用链；提出的控制性
问题已按上表修正并定向复查，复查未发现该修正范围内剩余必须修改项。审查始终为静态
文档范围，未构建或运行产品测试。

本轮检查文档结构、本地链接、名称示例、FR/SC 映射和 T001–T005 状态。五项均
`NOT_STARTED`。活动 feature 仍为 `specs/253-test-fixtures`，审查分支为 `spec253`。
