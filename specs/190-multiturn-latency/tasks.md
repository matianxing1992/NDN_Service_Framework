# Tasks: Multi-turn Token Generation Latency

**Status**: PLANNED
**Input**: [spec.md](spec.md), [plan.md](plan.md), [design contract](contracts/design.md)

## Current Checkpoint

2026-09-22 02:27 -05:00：已建立r260耗时与源码分析、需求、设计及7个行为任务，完成结构检查与两次独立只读审计修订；
ACK60秒已确认，FINALIZE约30秒有源码对应但丢失/拒绝原因待T004定位。
本轮只创建计划，不实施、不编译、不启动新模型。文档规划审计PASS见[audit.md](audit.md)，全部产品任务保持NOT_STARTED。
下一步T001建立准确分段基线，再T002验证1000ms ACK；不继续Spec189 r261独立实现。

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Phase timing](#t001-phase-timing) | NOT_STARTED | — | research已有历史分段，新增C++指标/回归未实现 | 2026-09-22 02:16 -05:00 |
| [T002 ACK window](#t002-ack-window) | NOT_STARTED | T001 | 1000ms目标，原生认证/截止回归待做 | 2026-09-22 02:16 -05:00 |
| [T003 Live turns](#t003-live-turns) | NOT_STARTED | T001 | 同handle/实时事件待实现 | 2026-09-22 02:16 -05:00 |
| [T004 Finalize and drain](#t004-finalize-and-drain) | NOT_STARTED | T001,T003 | 先定位控制闭环首边界，再限定修复 | 2026-09-22 02:16 -05:00 |
| [T005 Resident session](#t005-resident-session) | NOT_STARTED | T001 | 从Spec189 R261承接，真实ORT与owner验证待做 | 2026-09-22 02:16 -05:00 |
| [T006 Convergence and candidate](#t006-convergence-and-candidate) | NOT_STARTED | T002,T003,T004,T005 | C++oracle、preflight mutation、收敛门待做 | 2026-09-22 02:16 -05:00 |
| [T007 Matched experiment](#t007-matched-experiment) | NOT_STARTED | T006 | 三组配对和全部SC待验收 | 2026-09-22 02:16 -05:00 |

## Shared Execution Contract

每项包含反例/实现/只读静态审查/定向验证/证据，不机械拆成行政子任务。
批次、五lane、dynamic profile与收敛门引用[plan.md](plan.md#logical-batch-quality-plan)。
以下测试target/selector及新test文件均为planned，注册/链接是对应任务的一部分，不能宣称已有命令通过。
Native assertion/fixture/oracle均C++；Python仅启动和配置；共享业务生产路径，不写fake ACK或fake runner当模型证明。
每批记录一个 `evidence/b190-0N.md`，包含Review trace、Coverage matrix、Closure decision、
四类miss、build计时和实际结果；没有实测不勾选。失败保留raw并更新docs/failure-log.md。

## Phase 1: Observable Baseline

### T001 Phase Timing

- [ ] T001 Deliver correlated phase timing in `NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.cpp`, `ndn-service-framework/ServiceUser.cpp`, and `tests/unit-tests/spec190-timing.t.cpp`.

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

## Phase 2: User Story 1

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
**Exit**：定向C++通过，真实1秒成功率交T007；超时诊断先保留首边界，不自动重试延长。Batch B190-02。

## Phase 3: User Story 2

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
**Exit**：C++driver与fixture闭合，同模型同handle证明交T007；不靠checkpoint文件重开新Conversation通过。Batch B190-03，asan-ubsan。

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
fixture显式拥有Face/io/scheduler直到worker join，重复20次。MiniNDN健康checkpoint→Provider terminal目标≤2秒，
达不到须按事件解释并修复，不能以压缩超时通过。
**Exit**：有首边界证据和生产回归，保持持久提交含义；真实时延交T007。Batch B190-04，asan-ubsan。

## Phase 4: User Story 3

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
**Exit**：native fixture通过、owner计数闭合；Qwen驻留命中/资源趋势交T007。Batch B190-05，asan-ubsan，
另跑有界TSan `ResidentSessionConcurrency` 覆盖acquire/release/evict/close/single-flight竞态（plan定义范围/预算）；
加密临时backing和CUDA bypass记录，不声称其驻留已支持。

## Phase 5: Convergence and Acceptance

### T006 Convergence and Candidate

- [ ] T006 Deliver a closed candidate and native latency oracle in `examples/Spec189TwoProviderOracle.cpp`, `examples/wscript`, `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, and `specs/190-multiturn-latency/evidence/b190-06.md`.

**Outcome / owner**：生产入口与设计一致，oracle辨别真流式/真session复用/正确KV；错误候选不得启动。
**Read**：所有CD/FR/SC及T001–005证据；当前安装工具和launcher preflight；原Spec189协议oracle。
**Write scope**：复用oracle规则的Spec190薄入口（新文件时登记实际路径）、Waf、C++oracle反例fixture、
必要preflight mutation tests、active文档/API对应变化；不新建泛化平台或容器pipeline。
**Design binding**：CD-05；新target有definition/link/install映射；签名/API/双PDF仅同步本Spec已实现变化。
**Steps**：先写假profile/假cachehit/假token/错attempt/缺KV/缺终态反例→接入共享C++oracle→
冻结源/运行/输入/配置身份→错hash/错config/缺依赖零启动检查→只读CodeGraph+源审查完整五lane→修正→重审。
**Acceptance**：原生focused selectors与适用spec189回归、动态owner门、安装身份通过；语义/安全/owner/证据缺口为零；
记录PASS只指design-code convergence与preflight，不是性能PASS。有未解决控制性缺口则BLOCK T007。
**Exit**：一个不可变candidate READY_FOR_EXPERIMENT；所有partial依赖明确，不能跳过T004或T005。Batch B190-06。

### T007 Matched Experiment

- [ ] T007 Validate matched three-turn performance using `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, the installed `spec190-multiturn-oracle`, and `specs/190-multiturn-latency/evidence/b190-07.md`.

**Outcome / owner**：以真实两节点三轮输出/KV/时间/资源共同证明改善，不只报测试数。
**Read**：T006已冻结candidate、quickstart、CD-05、r260输入/采样和raw；不重新生成模型资产。
**Write scope**：新run roots与本批evidence/tasks；不得运行中修源码、换安装库或改候选。
**Steps**：验证系统安装闭包→先一组匹配smoke，失败停止并保留首边界→通过后至少3组配对，
控制组/处理组交替→用C++oracle逐轮检查token/事件/KV/load/FINALIZE→统计全部时延和资源→清理本次临时进程。
**Acceptance**：SC-001–006、T004≤2秒正常finalize门；失败不丢样本；若累积真实请求观察不足60秒，
追加相同样本，不延长单轮sleep；记录每轮首token、decode、checkpoint、terminal/退出及总时长，解释13秒残差归属。
**Exit**：仅满足全部目标才标Spec190性能PASS；否则PARTIAL，定位并回到所属任务；不因此关闭Spec189 full Repo资格。Batch B190-07。

## Dependencies and Strategy

`T001 → {T002, T003, T005}; T003 → T004; {T002,T004,T005} → T006 → T007`。
没有按文件拆分测试/实现/证据任务；7个任务各有独立行为或真实验收出口。
MVP：T001/T002先消除已知60秒等待；接着T003/T004保证真实交互与及时收尾；T005再减少加载开销。
新spec规划完成不勾选任何产品任务。无外部approval依赖；T004先诊断再补设计是技术前置，不要求用户代替定位。
