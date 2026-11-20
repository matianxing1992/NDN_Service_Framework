# Tasks: Semantic NDNSF-DI Data Naming

**Status**: IN_PROGRESS
**Progress Timestamp**: 2026-11-20 17:30 -06:00
**Design binding**: [spec.md](spec.md)、[plan.md](plan.md)、[naming](contracts/naming.md)、[integration](contracts/integration.md)。

用户已激活全量执行。原 T001 和 T002 按既有审计结论合并为当前 T001，其余顺移，映射见 audit。
每项以本单元的 native 验证和证据为完成依据，按依赖推进；push 另需授权。
跨机器接手时从 T001 的诊断验证继续。构建目录与临时日志未随 Git 转移，需在接收机上
构建并验证 `naming-static-publication-asan`、`model-contracts-asan` 和 `uav-large-fetch-lifetime-asan`。

## Execution Progress

| Task / batch | Status | Dependencies | Evidence / remaining | Updated |
| --- | --- | --- | --- | --- |
| T001 / B1 | IN_PROGRESS | activation | 无坐标回退、Owner 旧键、旧 canonical root 与 catalog 绑定 material 已验证。最新普通候选通过输入准入 3 用例 / 28 断言、真实 stored capsule Provider（含过期拒绝）341 断言、原包 18 用例 / 1,920 断言。交接时 ASan/UBSan 结果待验证。 | 2026-11-20 17:30 -06:00 |
| T002 / B2 | IN_PROGRESS | T001 | **审计 AUD-fcc593a0 第 3 条(b) 已修**（`de94f27d`）：旧 journal（无 `semanticPublication`）在默认入口带语义事实续用时不失败，按旧契约复用原前缀与旧对象名；带坐标的包仍要求坐标一致。验证：`StoredModelCapability` 49686、`DiPreparation` 310 断言。**未做**：Core durable lookup 的跨形状比较（审计第 3 条(a)）。 | 2026-11-20 09:35 -06:00 |
| T003 / B3 | PARTIAL | T001 | 已完成：坐标/名字/校验双读/投影/准入证据/输入名单组件（`c36433aa`）/phase 推导（`791e5c5b`）/`<R>` 语义+框架事实（`1f84b057`、`7ba91851`）。**未完成（审计 AUD-f3634504）**：`<R>` 缺保护实例（capability/epoch-key，投影层无来源，需保护发布 owner 在密封前提供）；稳定公开请求 ID；feedback 首包轮次 `round/1→0`；step 层区间（step 规范无层字段，需 adapter 认证的子图层跨度）。 | 2026-11-20 13:56 -06:00 |
| T004 / B4 | IN_PROGRESS | T001, T003 | 命名/grant/restore/坐标/重算/链路修复完成；**Saved codec N5 直接覆盖**（`e1000141`）；**capsule 与 packet root 的 AAD 绑定已完成**（`92b1d602`：可选 `packetRoot`，Owner 与目标两侧均取自 saved 记录，空根保持旧 AAD）。验证：`Spec221ProtectedKvCheckpoint` 720、`Spec221ProtectedKvPackets` 186、`Spec221KvSave` 157、`Spec221KvRestore` 73。**待做**：真实 Source→Owner→Repo 重开→目标恢复的端到端 fixture。 | 2026-11-20 14:35 -06:00 |
| T005 / B5 | NOT_STARTED | T001, T002, T003, T004 | 冻结候选安装后MiniNDN CPU cold/warm资格 | 2026-11-13 09:19 -06:00 |

## Implementation and Qualification

- [ ] T001 Migrate canonical naming metadata, protected publication and admission together.

  **Gap/exit**：C1/C2的metadata/codec、semantic stem、表示尾、manifest与真实caller在同一
  单元迁移；保留旧canonical闭集读法和原包，不提交会先破坏旧publisher的全局API语义。
  **Files**：Core ProducerDataNaming/PublishedContentName/ProtectedBlobManifest/
  ProducerSignerPolicy、ServiceUser/Provider，NativeCanonicalArtifactPublisher/Assembler、
  `NativeCanonicalRolePreparer` 和现有 ONNX 组装记录的 owner，
  NativeInferenceClient、NativeExecutionPlanJson固定source校验、model-object-keys、
  相应薄绑定、原native fixtures和Waf注册；不是逐文件机械改动要求。
  **Design binding**：C1/C2、N2/N3/N4；FR-001/002/003/004/006/007；SC-001/002/003/005。
  **Independent checks**：普通对象metadata和手工TLV向量/薄绑定wire一致，类型/UTF-8/64-byte/
  1024-byte边界拒绝；生产publish→验证→解密/组装小对象，不能只测prefix helper。
  新manifest只由Core生成统一root，旧override只读原记录；新旧closed codec、durable
  receipt exact复用、payload/完整语义/权限比较、同名冲突与原wire重发全部验证。
  capsule必须走nativeInstallModelObjectKeys→真正object fetch，覆盖fixed-vs-capsule及
  canonical-vs-stored互换、wrong recipient/boot/expiry；API-only项不新增publisher。
  同一小型 Qwen 源图用现有配方对照 PP/TP/hybrid，已有 YOLO/automatic fixture 验图分区
  坐标，检查两种加权布局在相同
  degree/rank 下产生不同标签。独立 TLV oracle 及生产接纳拒绝错层端点、错 entry/rank、
  错 cut 或布局冒用。核对现有本地组装记录能依据 N4 区分实际碎片，并继续读取同一份
  canonical 原包。
  有既有独立制品发布调用时覆盖其 manifest→exact fetch→验证链；仅有 codec/API 的类别
  沿原入口验证，不接入新的线上发布。复用 Spec253 隔离 fixture，真实模型用显式资格输入。
  StoredModel专用package/object/index/root向量归T002，不反向依赖。
  **Targets/risk**：C6的component/canonical/protected目标；serializer、持久原包和生命周期
  风险沿原native/ASan-UBSan fixture闭包有界验证。真实接口、target/TU与oracle字段对照在
  本项闭合，不把原本必须一起通过的codec和publication拆成两个先后DONE任务。

- [ ] T002 Migrate StoredModel as one reusable package publication.

  **Gap/exit**：C3的prefix/object/key/AAD/journal/index/root/read一体接线，包级一个
  publication identity，准备方离线后可使用；保留实际需要的既有exact handle。
  **Files**：`stored-model-capability.hpp/.cpp`、`stored-model-preparation.hpp/.cpp`、
  `stored-model-repository.hpp` 的引用路径与现有StoredModel/Repo native fixtures；
  仅在当前capsule引用确实受影响时同步 `model-object-keys.cpp`。
  **Design binding**：C3、N4 StoredModel；FR-004/006/007；SC-003/005。
  **Independent checks**：核对factory/journal分配prefix前已收到semanticPublication，不能从
  modelDigest猜ModelPath；新旧ready/partial journal按各自原record恢复，缺原数据不改旧state。
  小模型包生成并保存handle/journal，结束准备方、重开实际store/
  reader后按现有 RepoClient 路径取原wire；验证新/必要旧root、包内加密业务根与外层root、离线准备、当前User capsule、
  错root pin/对象名/密钥/recipient；不得用writer内存命中或bulk重发布代替。
  **Targets/risk**：C6 StoredModel+Repo目标；私钥文件、原包持久与取消清理，沿原fixture
  覆盖有界journal中断/恢复，不造新在线密钥服务或Data翻译服务。

- [ ] T003 Connect semantic runtime endpoints to exact IO, inputs and events.

  **Gap/exit**：C4的source-side事实/seed→`<RepresentationId>`→密封projection→producer与consumer相同
  exact名；拆开获取策略与对象identity，保留两种framing、bounded prefetch和原接纳。
  同步输入Selection/AAD与事件StreamBinding；核查原Core冻结/终态规则，有确证缺口才
  在原owner最小修补。
  **Files**：`NativeExecutionPlan.hpp/.cpp`、`NativeExecutionPlanJson.cpp`、
  `NativePlanProjectionBuilder.cpp`、`NativePlanSealer.cpp`、`dataflow-types.hpp`、
  `NativeProviderHandler.cpp`、`NdnsfCollaborationDependencyIo.cpp`、受影响tensor
  manifest codec、`ServiceUser.cpp`、`ServiceProvider.cpp`、`InvocationStream.cpp`、
  原production integration fixtures及Waf注册。
  **Design binding**：C4、C5保留Core段；N3/N5/N6；FR-004/005/007/008；SC-002/003/004。
  **Independent checks**：生产Catalog/fixed template/projection/makeEdge完整tuple对应后
  进入IO；仅对携带step模板的依赖在缺dependencyId/step/slot时于入队前拒绝，普通非step
  PP保留合法空step字段。public/core request身份在attempt1→2
  认证映射，旧attempt晚到事件拒绝，warm仍新request/attempt1。生产compute前后对照
  exact名；早EOS/stop仍沿预取的continuation名字执行，FINALIZE由认证lineage判定；
  feedback生产epoch e、消费e+1不偏移。PP、TP不同
  step/layer/rank/Producer、generic execute、共享output fanout；target slot不重命名
  output；合法estimate差异调整、超限/错误binding拒绝；inline MANIFEST携带segment0
  与普通模式均成功，发布后不能换framing；input小/大和End/Response一致性；取消drain。
  同一已接纳保护快照内改变合法本地timeout/window/estimate/locator后取回逐字节相同
  原wire；改变capability/硬授权预算或保护集合则新接纳/新表示，不能只断言Name相同。
  源码闭包另核对 `ProviderGroupCoordinator.cpp` 的KDF/nonce和
  `NdnsfCollectiveControl.cpp` 的AAD；保留现有算法，不预设密码协议改造。
  复用 Spec252 已有的共享受众与下游归约，核对同一源 slot 在多个已获准读者处使用同名
  原包、不同 Producer 的 partial 仍各有原名、下游 SUM 保持 `LOCAL_STEP`。覆盖错受众/
  slot、能力不足沿兼容计划和不同 `NativeGroupProjectionBuilder` 保护 operation 的隔离。
  per-edge ordinal 不成为新的 source step。
  **Targets/risk**：C6 runtime/event/Core目标；异步/不可信wire/重放风险，以现有真实Face/
  publisher/consumer fixture验证，保留Fixture外部owner寿命；不创建第二套transport或调度。

- [ ] T004 Migrate protected KV checkpoint names without redesigning its cipher.

  **Gap/exit**：C5在现有header-AAD seal之后算完整表示commitment，投影checkpoint
  conversation/epoch/role/layout；packet root、reference/capsule与Repo restore一致。
  **Files**：`protected-kv-checkpoint.*`、`protected-kv-packets.*`、
  `native-kv-publication-repo.cpp`、`native-kv-restore-repo.cpp`、实际export/receipt调用方
  与现有native-protected-kv fixtures。
  **Design binding**：C5、N5 checkpoint；FR-001/004/006/008；SC-003/005。
  **Independent checks**：certificate-aware packet helper计算`<CheckpointPrefix>`，grant dataPrefix取packet
  root前缀而非底层byte-reference.objectName；Owner加密证书与Provider签名证书不混同。
  实际小KV导出/保护/存储，退出writer并重开store后恢复；核对原
  Producer/原wire、certificate/FinalBlock与epoch/layout；wrong recipient/pin、旧epoch、
  损坏和取消负例；新Provider恢复不改旧包名。不伪造额外KV页或全局conversation Data。
  **Targets/risk**：C6 KV/Repo目标；私有状态/解析/lifecycle，沿原native fixtures有界验证。

- [ ] T005 Qualify the installed two-Provider CPU inference path.

  **Gap/exit**：激活后冻结T001–T004组合source/config/binary身份，生产call-chain审查
  无控制性缺口，Waf安装后从源码目录外执行原MiniNDN launcher与native输出/KV oracle。
  **Files/owner**：原MiniNDN deployment/verification入口和必要原生fixture，证据放本
  Spec现有 `evidence/`；无需要则只运行真实生产路径，不为任务写新runner。
  **Design binding**：C6、plan W01–W09；FR-009；SC-001–SC-006。
  **Independent checks**：两Provider Qwen3-0.6B INT4 CPU；cold25→32，warm新增24、
  起始context81→32；每次核对输出/KV和原包复用、正确Producer/phase/step，非强制
  cleanup。已有TP支持范围沿原生产regression，不实现尚缺TP功能。
  **Evidence**：source/config/model/installed binary identity、实际命令、PASS/FAIL首边界，
  cold/warm壁钟、ONNX时间、payload/wire bytes、包数/Name开销与已有latency指标；
  静态、native、安装与实验结果分别记录。无配对实测不报告性能增益。

## Execution Rules

每项采用plan的原批次出口与仓库共享静态/动态规则；未运行记NOT_RUN，PARTIAL不算DONE。
激活后若发现实际bug，先在固定 `origin/KVCache` 对应实现/回归中查既有解法，再改当前owner；
没有对应实现则记录查找范围，不为此新造状态机。本稿未授权修复代码或启动验证。
