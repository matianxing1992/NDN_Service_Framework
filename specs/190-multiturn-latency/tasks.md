# Tasks: Multi-turn Token Generation Latency

**Status**: PARTIAL
**Input**: [spec.md](spec.md), [plan.md](plan.md), [design contract](contracts/design.md)

## Current Checkpoint

2026-09-22 04:33 -05:00：严格串行完成 T001；T002 及后续任务仍未启动。
旧→新：T001–T005不变；旧T008→T006、旧T009→T007、旧T010→T008、旧T011→T009、旧T006→T010、旧T007→T011。
Batch ID/evidence路径保持原身份，历史提交/审计不改写；下表与正文使用新任务ID。
T001 的实现、C++ focused regression、compile-link 和只读静态复核已闭合；证据见
[b190-01](evidence/b190-01.md)。本项不包含 MiniNDN/Qwen 两节点实验或性能验收。
下一步只有 T002，不能跳过或并行推进后项。

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Phase timing](#t001-phase-timing) | DONE | — | [b190-01](evidence/b190-01.md)；216/216 compile-link，`Spec190Timing` C++ regression 3/3，静态复核 PASS；MiniNDN/Qwen/performance unobserved | 2026-09-22 04:33 -05:00 |
| [T002 ACK window](#t002-ack-window) | NOT_STARTED | T001 | 1000ms目标，原生认证/截止回归待做 | 2026-09-22 03:00 -05:00 |
| [T003 Live turns](#t003-live-turns) | NOT_STARTED | T002 | 同handle/实时事件待实现 | 2026-09-22 03:00 -05:00 |
| [T004 Finalize and drain](#t004-finalize-and-drain) | NOT_STARTED | T003 | 先定位控制闭环首边界，再限定修复 | 2026-09-22 03:00 -05:00 |
| [T005 Resident session](#t005-resident-session) | NOT_STARTED | T004 | 从Spec189 R261承接，真实ORT与owner验证待做 | 2026-09-22 03:00 -05:00 |
| [T006 Stage transfer](#t006-stage-transfer) | NOT_STARTED | T005 | actual bundle/wire字节与多发修复待做 | 2026-09-22 03:00 -05:00 |
| [T007 Persistent Repo owner](#t007-persistent-repo-owner) | NOT_STARTED | T006 | 固定根、单owner、恢复/cleanup隔离待做 | 2026-09-22 03:00 -05:00 |
| [T008 Query and reuse](#t008-query-and-reuse) | NOT_STARTED | T007 | 接既有manifest-first/root-last，免重复STORE待做 | 2026-09-22 03:00 -05:00 |
| [T009 Protected material reuse](#t009-protected-material-reuse) | BLOCKED | T008 | 先冻结crypto-owner恢复/key-reference/retention接口，禁止直接移除protected miss门 | 2026-09-22 03:00 -05:00 |
| [T010 Convergence and candidate](#t010-convergence-and-candidate) | NOT_STARTED | T009 | 新增真实Repo/重启/字节预算oracle与preflight；门未执行 | 2026-09-22 03:00 -05:00 |
| [T011 Matched experiment](#t011-matched-experiment) | NOT_STARTED | T010 | 三组warm配对、cold/三次restart/缺层及SC-001–009待验收 | 2026-09-22 03:00 -05:00 |

## Shared Execution Contract

每项包含反例/实现/只读静态审查/定向验证/证据，不机械拆成行政子任务。
批次、五lane、dynamic profile与收敛门引用[plan.md](plan.md#logical-batch-quality-plan)。
以下测试target/selector及新test文件均为planned，注册/链接是对应任务的一部分，不能宣称已有命令通过。
Native assertion/fixture/oracle均C++；Python仅启动和配置；共享业务生产路径，不写fake ACK或fake runner当模型证明。
每批记录一个 `evidence/b190-0N.md`，包含Review trace、Coverage matrix、Closure decision、
四类miss、build计时和实际结果；没有实测不勾选。失败保留raw并更新docs/failure-log.md。


## Phase 1: T001

### T001 Phase Timing

- [x] T001 Deliver correlated phase timing in `NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.cpp`, `ndn-service-framework/ServiceUser.cpp`, and `tests/unit-tests/spec190-timing.t.cpp`.

**Outcome / owner**：Core/DI/CLI各自产生阶段事件，可以区分网络、认证、规划、模型计算、交付及终态等待。
**Read**：research.md、CD-01、现有RuntimeTiming、NativeInferenceClient::beginCoreRequest/commitConversationTurn、r260 raw。
**Write scope**：上述文件及相应.hpp（仅必要声明）、NativeInferenceClient.cpp、ORT adapter真实run计时、
examples/DI_NativeRequester.cpp、tests/wscript；不重做通用logging框架。
**Design binding**：CD-01 + TurnTiming；现有业务签名不改，字段owner按data-model；Design status: proposed。
**Steps**：核对日志身份→C++ fixture定义事件顺序/遗漏/重复反例→接入真实发布/验证/run/交付点→
独立C++解析校验本地持续时间→冻结静态复审→增量构建/回归→保存baseline字段缺口，不重跑昂贵模型复现已知60秒。
**Acceptance**：`spec190-latency-tests / PhaseTiming` 拒绝跨request/attempt拼接、缺阶段不能报0、steady计时不受wallclock回拨影响；
真实run计数不含warmup，首token和checkpoint不能混淆。五lane与新target定义/链接闭包均登记。
**Exit**：指标和C++回归可用；性能改善NOT_CLAIMED。Batch B190-01，风险低，dynamic none。

## Phase 2: T002

### T002 ACK Window

- [ ] T002 [US1] Enforce the one-second Qwen ACK profile with verified closure regressions in `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, `examples/DI_NativeRequester.cpp`, and `tests/unit-tests/spec190-ack-window.t.cpp`.

**Outcome / owner**：DI profile实际采用1000ms，Core维持原认证、截止与候选快照契约。
**Read**：CD-01；ServiceUser::handleAckCollectionTimeout、BeginCollaborationWithProviders、NativeOfferAdmission；T001事件。
**Write scope**：launcher的预算/arg配置、C++ requester配置读取和生效值输出、原生test/Waf；
只有定向反例证明原生超时路径缺陷时才改NativeInferenceClient/ServiceUser，并明确Changed gate。
**Design binding**：CD-01；不改变通用5000ms默认、不新增提前关闭算法、不在ACK前加载模型。
**Steps**：消除模型大小绑定ACK窗口→覆盖显式值传递/非法值→C++可控时钟触发发布和冻结→
检查首次/续轮认证路径→静态复审后限定构建/测试。
**Acceptance**：`AckWindow`：1000ms生效；0/负/大于总deadline拒绝；999/1000/1001ms、late/duplicate/invalid/negative ACK、
认证未完、cancel/deadline同时触发，冻结一次且不可变；缺角色无Selection；原grant/offer校验无绕过。
**Exit**：定向C++通过，真实1秒成功率交T011；超时诊断先保留首边界，不自动重试延长。Batch B190-02。

## Phase 3: T003

### T003 Live Turns

- [ ] T003 [US2] Stream events during execution and reuse one native Conversation in `examples/DI_NativeRequester.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.cpp`, and `tests/unit-tests/spec190-live-turns.t.cpp`.

**Outcome / owner**：原生CLI真实边生成边消费，在同一Runtime/PreparedModel/Conversation依次完成三轮。
**Read**：CD-02；PreparedModel.hpp里的RequestHandle/EventReader、Conversation::State、launcher逐轮start流程。
**Write scope**：CLI、Conversation.cpp及必要PreparedModel.cpp生命周期修复、launcher turns配置与启动接线、test/Waf；
不新增Python对话实现、不新增服务模式/UI。
**Design binding**：CD-02；单轮兼容、turns数组契约；每轮当前options/generationId/输入与前轮checkpoint绑定。
**Steps**：C++可阻塞fixture验证事件先于terminal→接入实时读取/flush→数组driver循环复用对象→
验证立即下一轮与真正并发busy边界→launcher一次启动driver→静态复审/回归。
**Acceptance**：`LiveTurns`：首token在terminal之前；无丢失/重复/乱序；EOS/EOT/预算均停止；三轮不同requestId但同对象；
第二轮失败后无第三轮；取消仍释放；result后立即request不偶发busy；真正同时request拒绝。
必须有C++父进程/pipe调用真实CLI，在后续生成被fixture暂停时能读到首事件；测试暂时空队列不当EOF、
失败/取消没有正常terminal也有界退出，不能只用EventReader内存fixture证明flush。
**Exit**：C++driver与fixture闭合，同模型同handle证明交T011；不靠checkpoint文件重开新Conversation通过。Batch B190-03，asan-ubsan。

## Phase 4: T004

### T004 Finalize and Drain

- [ ] T004 [US2] Close the authenticated FINALIZE lifecycle in `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, and `tests/unit-tests/spec190-terminal-drain.t.cpp`.

**Outcome / owner**：健康轮不等满30秒补偿窗；持久commit、rollback补偿、失联恢复及最终drain正确。
**Read**：CD-03；publishConversationControls、finalizeProviderState、waitConversationPromotion、Core协作发布服务owner；
r260 checkpoint→terminal 29.9秒证据。此项不是“将30000常量改小”。
**Write scope**：上述DI文件、必要Conversation coordinator/Runtime owner、已确认有缺陷的Core通用owner（若触及需补Core回归），test/Waf。
**Design binding**：CD-03；诊断子步骤可执行，生产patch须先按实际首边界细化FN/owner；当前不能称生产修复已ready。
**Steps**：T001关联COMMIT发布/验证/ack、durable journal commit、FINALIZE发布/服务存活/接收/接受→
找首次缺失或拒绝原因→补最小C++重现→修发布生命周期/身份/处理或唤醒，而非删barrier→复审/回归。
若同handle已完全解决，只保留必要诊断与回归，不强造Core重构。不得直接增加新握手协议掩盖现有控制消息未送达。
**Acceptance**：`TerminalDrain`：正常FINALIZE提前结束等待；丢失FINALIZE已commit的KV保留到原expiry；
未commit超时rollback；重复/乱序/错身份不双提交；journal失败补偿；关闭中迟到回调安全；drain返回结果真实。
必须覆盖两角色仅一侧COMMIT成功/另一侧commit ACK丢失：不成功checkpoint、不进入下一轮、已提交侧补偿；
与持久commit之后丢FINALIZE保留KV的情况严格区分。
fixture显式拥有Face/io/scheduler直到worker join，重复20次；原生受控时钟验证健康FINALIZE不依赖补偿超时推进，
真实owner回调完成/退出与异常保留期限均在本项测试通过。
**System acceptance owner**：MiniNDN健康checkpoint→Provider terminal≤2秒由T011完整负责，SC-003/005门不变；
达不到须重开本项修复并重验受影响候选，不能以压缩超时通过，也不能称系统指标已由本项fixture证明。
**Exit**：有首边界证据和生产回归，保持持久提交含义；真实时延交T011。Batch B190-04，asan-ubsan。

## Phase 5: T005

### T005 Resident Session

- [ ] T005 [US3] Reuse bounded CPU loaded sessions with isolated request evidence in `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeSessionCache.hpp`, `OnnxRuntimeModelRunner.cpp`, and `tests/unit-tests/spec190-resident-session.t.cpp`.

**Outcome / owner**：Provider-owned CPU session复用，旧ctx/grant/KV/profile不会成为新请求状态；退出实际释放。
**Read**：CD-04/data-model；NativeRunnerPreparation、NativeCanonicalOnnxAssembler已产生的digest；ProviderArtifactCache的metadata-only契约。
**Write scope**：新cache.hpp/.cpp、同目录OnnxRuntimeModelRunner.hpp/.cpp、DI/ExecutionEvidence.hpp/.cpp、
DI/Provider.cpp、examples/DI_NativeProviderExecutable.cpp、必要factory接线、C++fixture与Waf；不迁移通用artifact cache。
**Design binding**：CD-04；fresh wrapper + shared load owner；原单参数入口保持；独立request与load证据。
**Steps**：C++自生成小ONNX/外部权重fixture→完整key与lease状态→single-flight/idle清理/close→
fresh evidence与真实ORT调用→Provider host/库默认接线→显式disabled控制→逐项静态复审及组合测试。
**Acceptance**：`ResidentSession`实际ORT两次输出正确、同key只加载一次；key每个关键字段变更miss；
并发/TTL/换模型/evict/close/loading-failure/cancel均不泄漏或UAF；失效授权即使已有session也拒绝；
旧profile不能改request/attempt，重复EndProfiling反例；所有临时ONNX由RAII清理。
首次loader请求取消但另有合法waiter、全部waiter取消、close后load迟到完成均有C++反例，
不得借发起请求ctx延长生命或重新插入已关闭cache。
并发evict/acquire、在用项retire拒绝新租用、close后禁止cold fallback、drain超时/最终成功均验证；
1slot默认只作用本profile的显式resident配置，原无cache入口和其他profile行为不变。
**Exit**：native fixture通过、owner计数闭合；Qwen驻留命中/资源趋势交T011。Batch B190-05，asan-ubsan，
另跑有界TSan `ResidentSessionConcurrency` 覆盖acquire/release/evict/close/single-flight竞态（plan定义范围/预算）；
加密临时backing和CUDA bypass记录，不声称其驻留已支持。

## Phase 6: T006

### T006 Stage Transfer

- [ ] T006 [US4] Bound stage data transfer in `NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.cpp`, `NdnsfCollaborationDependencyIo.cpp`, and `tests/unit-tests/spec190-stage-transfer.t.cpp`.

**Outcome / owner**：按真实tensor/encoded/wire预算发送当前stage所需数据，不多发KV/权重/完整logits；材料流量观察点可被独立校验。
**Read**：CD-06、outputForEdge/withoutProviderLocalState、lastLogits/makeTokenFeedback、dependency publish/prefetch、T001事件。
**Write scope**：上述DI路径、NativeEpochCoordinator.cpp、RuntimeTiming、必要Core分段/wire观察点、test/Waf；
额外mask/position删除必须同时改sealed边契约和接收重建，不能仅改发送端。
**Design binding**：CD-06；保留业务签名，统一identity计数，counter累计与delta明确；Design status: proposed。
**Steps**：扩展既有Provider-local KV C++fixture捕获实际bundle→构造prompt/delta/decode/finalize动态输入→
独立解码/计算shape字节→接入生产wire/retry/本地copy分层指标→仅修有反例证明的多发/复制→冻结复审/回归。
**Acceptance**：`StageTransferBudget`注入额外KV、weights、full logits、重复bundle、缺失position/lineage和乱序必须检出；
实际tensor集合及数值与sealedcontract一致，允许合法metadata/重传但单独计；cumulative snapshot不能重复累计。
layer/assembled/resident三个状态分列，C++fixture注入已知非零材料传输验证计数，不以尚未实现的热缓存作本项前置。
**Material acceptance owner**：热材料零payload与缺对象精确补取完整归T009原生生产回归及T011真实系统验收，
SC-007/009保持不变；T006只完成stage数据契约与流量计量，不宣称保护材料缓存已有命中能力。
**Exit**：C++预算/正确性反例闭合，未采集字段标unknown；不靠降低logging或断言network=0常量通过。B190-08。

## Phase 7: T007

### T007 Persistent Repo Owner

- [ ] T007 [US5] Reopen a fixed per-node Repo safely in `NDNSF-DistributedRepo/src/backends/FilesystemRepoStoreBackend.cpp`, `NDNSF-DistributedRepo/src/RepoNode.cpp`, and `tests/unit-tests/spec190-repo-restart.t.cpp`.

**Outcome / owner**：固定node根跨run/进程重启，单writer恢复可读committed数据；run cleanup不删除共享payload。
**Read**：CD-07、BackendOwnershipLease、recoverOrphans、RepoNode::registerServices、launcher prepare_fixed_workspace/cleanup_encrypted_repository。
**Write scope**：上述后端/Node仅补已发现缺口；C++ requester/Provider注入单owner与固定配置；launcher只配置根/启动，
禁止在Python补恢复/commit逻辑；native test及tests/wscript，现有安装target `ndnsf-distributed-repo`。
**Design binding**：CD-07；复用现有constructor/范围接口，node/deployment/root/owner稳定，boot/catalog状态新建。
**Steps**：先真实后端跨owner/进程close-reopen C++fixture→验证已有恢复/锁能力→接per-node固定根并排除reset/cleanup→
测试crash/半提交/双writer/损坏/满额和目录逃逸→复审后只编受影响Repo/调用者。
**Acceptance**：`RepoRestart`至少3次新PID复用同根，payload hash/count/bytes不变；所有committed对象可查读，
半提交不成为hit，第二writer BUSY，不夺锁；有效read期间不删对象；普通失败清理只清owned staging；
启动不将整权重库加载入内存，测试小ONNX/文件与子进程均RAII收尾。
**Exit**：原生存储/owner证明闭合，网络Repo服务及真实模型重启交T011。B190-09，asan+文件故障注入。

## Phase 8: T008

### T008 Query and Reuse

**Prepare-level requirement**：production `user.prepare(model)`在拆层/导出/打包前查Repo；完整命中直接复用已校验prepared receipt。
C++ fixture记录split/export/package/STORE计数：第二次调用及新进程重启后均为0，返回材料/IO契约与冷准备等价；
变化内容/配置必须miss，部分缺失仅重建缺失依赖闭包。hash/必要inspection与授权成本单列，不能先完整准备再只去重STORE。

- [ ] T008 [US5] Reuse verified committed publications before STORE in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.cpp`, and `tests/unit-tests/spec190-repo-lookup-reuse.t.cpp`.

**Outcome / owner**：User prepare查询完整publication identity，命中直接引用，避免重复ingest/分层/存储及大型vector物化。
**Read**：CD-08；RepoSourceProvider::load/publish和已有Spec189RepoPublication回归；Runtime.hpp两个Repository port、RepoClient::requestManifest。
**Write scope**：既有adapter及Runtime.hpp/.cpp新增lookupPrepared可选port、NativeCanonicalArtifactPublisher必要接线、
ModelPreparationCache.cpp/.hpp前置lookup与恢复、PreparedModelPackage.hpp版本化元数据接线、NativeRequestCatalog factory引用恢复、
examples/DI_NativeRequester.cpp、C++fixture/Waf；不复制另一个Repo publisher。
恢复schema/校验/当前adapter与runtimeBinding owner按CD-08；真实冷生成计数为正、热生成计数为零，轻量wrapper重建另计。
**Design binding**：CD-08；稳定完整identity替代仅sourceDigest根；旧port默认nullopt兼容，不改原请求授权/Selection。
**Steps**：已有首写/复用回归扩到真正close-reopen→完整identity/key冲突反例→小manifest先查的native快路径→
miss才bounded publish/root-last，命中保留rollbackOwned=false→复审/构建/定向测试。
**Acceptance**：`RepoLookupReuse`第二run STORE/新payload/materialization计数0；请求元数据/本地hash读另计；
同source不同initializer/profile/layer不可错误命中；缺子对象只补缺失；unauthorized/conflict/unavailable不当miss盲写；
lookup/publish竞争、取消不删他人已提交对象，误清理旧run材料反例必须失败。
**Exit**：native canonical复用通过，不据此声明protected密文/网络serving复用已实现。B190-10，asan+事务故障矩阵。

## Phase 9: T009

### T009 Protected Material Reuse

- [ ] T009 [US5] Preserve current authorization during durable material reuse in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp`, `ndn-service-framework/ServiceUser.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`, and `tests/unit-tests/spec190-protected-material-reuse.t.cpp`.

**Outcome / owner**：真实Repo路径新run新grant合法复用既有材料/组装产物；不靠临时目录保留或compatibility bypass骗过验收。
**Read**：CD-09；Core publishEncryptedLargeData/EncryptedLargeDataRangeStore，Source析构清理、当前grant/host plaintext lease、assembler protected-miss门。
**Write scope**：上述Core/Repo/DI owner及必要hpp、NativeProtectedProvider/runner factory接线、test/Waf；不重做加密算法。
**Design binding**：CD-09，当前BLOCK生产编码；先在契约冻结durable key-reference/serving恢复、新grant绑定及retention公共签名、
owner/错误/取消/失效流程，标明与原request-scoped API兼容；独立只读审查后才解除该gate。
**Steps**：复用现有crypto-owner而非在Repo藏key→完整加密identity/receipt与恢复事务→显式durable与transient清理分离→
当前Selection后校验材料/assembled命中→缺层走原保护fetch→旧grant/key/boot等C++反例→冻结组合审查/回归。
**Acceptance**：`ProtectedMaterialReuse`旧/错grant、失效key、错AAD/ciphertext、非法保留policy仍拒绝；
合法restart后真实Repo lookup/read/serving可用，相同protected identity大payload不重复STORE；
材料/assembled热命中零material网络payload，缺一对象只取该role必要范围；保留原副本安全与迟到清理边界。
Provider新boot必须新session，旧KV receipt不能命中；不能把T005明文diagnostic通过计为本任务通过。
**Exit**：新安全契约和生产回归均完成，真实Qwen证明交T011；接口未闭合保持BLOCK，不删除原安全门。B190-11。

## Phase 10: T010

### T010 Convergence and Candidate

- [ ] T010 Deliver a closed candidate and native latency oracle in `examples/Spec189TwoProviderOracle.cpp`, `examples/wscript`, `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, and `specs/190-multiturn-latency/evidence/b190-06.md`.

**Outcome / owner**：生产入口与设计一致，oracle辨别真流式/真session复用/正确KV；错误候选不得启动。
**Read**：所有CD/FR/SC及T001–T009已完成证据；当前安装工具和launcher preflight；原Spec189协议oracle。
**Write scope**：复用oracle规则的Spec190薄入口（新文件时登记实际路径）、Waf、C++oracle反例fixture、
必要preflight mutation tests、active文档/API对应变化；不新建泛化平台或容器pipeline。
**Design binding**：CD-05–09；新target有definition/link/install映射；签名/API/双PDF仅同步本Spec已实现变化。
**Steps**：先写假profile/假cachehit/假token/错attempt/缺KV/缺终态反例→接入共享C++oracle→
冻结源/运行/输入/配置身份→错hash/错config/缺依赖零启动检查→只读CodeGraph+源审查完整五lane→修正→重审。
**Acceptance**：原生focused selectors与适用spec189回归、动态owner门、安装身份通过；语义/安全/owner/证据缺口为零；
记录PASS只指design-code convergence与preflight，不是性能PASS。有未解决控制性缺口则BLOCK T011。
还要验证Repo固定根不在任何cleanup目标内、单writer配置、现存数据不能当错误候选的可删除staging；
新增C++oracle必须检查真实Repo提交/查询/读取、restart、payload delta及实际wire，拒绝compatibility假通过。
**Exit**：一个不可变candidate READY_FOR_EXPERIMENT；T001–T009全部DONE，无partial依赖；本项oracle/preflight与收敛检查通过。Batch B190-06。

## Phase 11: T011

### T011 Matched Experiment

- [ ] T011 Validate matched three-turn performance using `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, the installed `spec190-multiturn-oracle`, and `specs/190-multiturn-latency/evidence/b190-07.md`.

**Outcome / owner**：以真实两节点三轮输出/KV/时间/资源共同证明改善，不只报测试数。
**Read**：T010已冻结candidate、quickstart、CD-05、r260输入/采样和raw；不重新生成模型资产。
**Write scope**：新run roots与本批evidence/tasks；不得运行中修源码、换安装库或改候选。
**Steps**：验证系统安装闭包→先一组匹配smoke，失败停止并保留首边界→通过后至少3组配对，
控制组/处理组交替→用C++oracle逐轮检查token/事件/KV/load/FINALIZE→统计全部时延和资源→清理本次临时进程。
**Acceptance**：SC-001–009、T004≤2秒正常finalize门；失败不丢样本；若累积真实请求观察不足60秒，
追加相同样本，不延长单轮sleep；记录每轮首token、decode、checkpoint、terminal/退出及总时长，解释13秒残差归属。
**Exit**：仅满足全部目标才标Spec190性能PASS；否则PARTIAL，定位并回到所属任务；不因此关闭Spec189 full Repo资格。Batch B190-07。

新增Repo-enabled验收按CD-06–09：同固定根首次cold提交、至少三次Repo/Provider进程重启、合法warm查询命中、
一次选中材料缺失的真实fetch；control/treatment均相同warm状态。磁盘目录/manifest/digest、STORE计数、
material payload/wire与session load独立核对；不能仅靠旧cache-compatible oracle通过。

## Dependencies and Strategy

**Execution mode**: STRICT_SERIAL。
T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011。
每个Depends都是完成依赖，不是仅实现/静态通过依赖；不允许[P]、独立任务先跑或跨任务暂缓测试。
每项内部按设计核对/必要诊断→实现及测试→只读静态审查→修复复审→受影响构建/原生回归/适用动态验证→证据与DONE闭合。
一个顶层任务对应一个可验收批次，内部步骤不是可跳转的顶层任务；共用构建树，不为串行而全树重编。
未完成、PARTIAL或BLOCKED时停在当前项修复；T004诊断与T009安全契约冻结先在本项闭合，不能跳去后项。
最终T011只拥有真实两节点系统/性能验收；T001–T009自己的原生回归、负例和生命周期门不得留给T010/T011补做。
局部DONE不等于SC或整Spec PASS；原有真实实验要求全部保留。若末项发现前项缺陷，重开最早受影响项，
暂停后项并修复/重验依赖闭包；这是受控失败恢复，不是正常执行顺序的来回跳转。
