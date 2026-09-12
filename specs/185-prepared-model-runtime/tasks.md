# Tasks: Prepared Model Runtime

**Status**: PLANNED | **Date**: 2026-09-12
**Input**: [spec](spec.md) · [plan](plan.md) · [C-01](contracts/public-api.md) · [C-02](contracts/preparation.md) · [C-03](contracts/execution.md) · [C-04](contracts/validation.md) · [C-05](contracts/api-usability.md) · [C-06](contracts/cpp-first.md)

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T015 Installed C++ API and ABI Closure](#t015) | NOT_STARTED | none | B0 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T001 Runtime Configuration and Export](#t001) | NOT_STARTED | B0 exit | B1 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T002 Runtime Shutdown and Child Ownership](#t002) | NOT_STARTED | T001 static | B1 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T003 Verified Package Preparation](#t003) | NOT_STARTED | B1 exit | B2 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T004 Single Flight Refresh and Leases](#t004) | NOT_STARTED | T003 static | B2 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T016 Extension Registration and Cooperative Control](#t016) | NOT_STARTED | B2 exit | B2E planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T005 Prepared Request Projection](#t005) | NOT_STARTED | B2E exit + Spec184 scoped dependency gate | B3 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T006 Handle Deadlines Events and Cancellation](#t006) | NOT_STARTED | T005 static | B3 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T007 Prepared Conversations and Committed Checkpoints](#t007) | NOT_STARTED | B3 exit | B4 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T008 Conversation Recovery Replacement and Export](#t008) | NOT_STARTED | T007 static | B4 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T009 Provider Facade and Authenticated Assembly](#t009) | NOT_STARTED | B4 exit | B5 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T010 Protected Artifact and Runner Template Reuse](#t010) | NOT_STARTED | T009 static | B5 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T011 Native Caller Migration and Compatibility Registry](#t011) | NOT_STARTED | B5 exit | B6 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T013 Current Candidate Process Qualification](#t013) | NOT_STARTED | B6 exit | B7 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T012 Thin Python Prepared Model Facade](#t012) | NOT_STARTED | B7 C++ qualification exit | B8 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |
| [T014 Design API and Scoped Handoff](#t014) | NOT_STARTED | T012 acceptance | B9 planned; implementation/build/runtime NOT_RUN | 2026-09-12 |

## Current Checkpoint

2026-09-12：扩展为完整API表面审计，明确独立C++ SDK和进程入口；Python仅包装同一native对象。
新增C-05/C-06及T015安装/ABI、T016扩展生命周期；16任务、11批全部NOT_STARTED。
[API审计](api-review.md)、[声明清单](api-surface-index.md)、[修订证据](evidence/api-review-20260912.md)。
原始[14任务规划证据](evidence/planning-20260912.md)保留为历史；本次不运行产品编译或资格测试。
下一执行单元T015；先核对当前HEAD/dirty diff，不把本次规划审查计为实现静态PASS。
Spec184未完成资格和既有Design 54文件漂移不因本次规划关闭。

## Shared Task Rules

路径缩写 `Runtime.cpp` 等未带前缀的DI文件均位于
`NDNSF-DistributedInference/cpp/ndnsf-di/`；tests/examples/pythonWrapper/Design路径从repo root解析。
原生行为任务先编写相应C++反例与fixture，再实现完整行为并用官方review-agent只读审查全diff及五lane。
同批任务静态通过、测试尚未运行时状态PARTIAL，不能勾选。B1–B5在该批第二任务后共享构建/测试；
B0、B2E、B6–B9是单任务批次，各自达到出口即验证。T012只写binding断言，T014只做文档交付。
跨批验收依赖必须实际通过；T012还要求T013完整C++ qualification出口，不接受仅静态接线。
成员证据采用C-04同一批记录模板，不创建第二份进度权威。
所有新生产公共方法补C-01英文Doxygen义务；涉及private owner/提交点补说明原因的英文注释。
目标路径须在task开始用CodeGraph/rg定位，若历史路径变动先更新本卡与symbol map，不能在未知文件下另建重复实现。

## Implementation Units

<a id="t001"></a>

- [ ] T001 [US1] Runtime Configuration and Export — Runtime.hpp/Runtime.cpp; root wscript; tests/unit-tests/di-runtime.t.cpp

  **Batch / Depends**: B1 / B0 exit。

  **Read / contract**: NativeInferenceClient constructors, NativeRequestRuntime, examples/DI_NativeRequester.cpp; C-01。

  **Implementation and review**: 提取现有operator配置组合为Runtime::open；校验profile、trust、所有资源上限；新增公开头安装/target map。RuntimeConfig.nativeConfigPath沿用当前native requester-v1配置；单用户default规则见C-01；taskContract独立绑定，不能从catalog猜测。

  **API revision**: C-05/C-06：启动冻结model key注册与精确配置；稳定application头不泄露advanced/native类型。

  **Exit / oracle**: 有效配置可创建User；非default profile拒绝、错误trust/零预算/初始化中途失败无残留；公开头消费链接成功。

<a id="t002"></a>

- [ ] T002 [US1] Runtime Shutdown and Child Ownership — Runtime.cpp; tests/unit-tests/di-runtime.t.cpp

  **Batch / Depends**: B1 / T001 static。

  **Read / contract**: SerialRequestExecutor, NativeInferenceClient::close, C-01 lifetime。

  **Implementation and review**: 按C-06实现原生drainAsync及可退订token；接入owner registry、close/drain和失败逆序清理；weak callback断环；补prepare/wait/drain的owner线程拒绝路径和最后owner释放流程。

  **Exit / oracle**: close幂等、回调中close无自join、drain超时可重试；外部Face/IO fixture显式寿命；TSan同矩阵两次通过。

<a id="t003"></a>

- [ ] T003 [US1] Verified Package Preparation — PreparedModel.hpp/PreparedModel.cpp; ModelPreparationCache.hpp/ModelPreparationCache.cpp; tests/unit-tests/di-preparation.t.cpp

  **Batch / Depends**: B2 / B1 exit。

  **Read / contract**: NativeRequestCatalog/NativeCanonicalPreparationCatalog/NativeRequestPreparation; C-02。

  **Implementation and review**: 实现User::prepare的owned source路径及既有受保护fetch接线；校验配置/双graph/initializer/adapter；将冻结catalog收进Package，构造后才发布。补独立内容oracle和生产解析计数。

  **API revision**: C-05/C-06：普通prepare("default")与原生prepareAsync共享唯一owner；只读capabilities/输入输出schema；高级PrepareRequest另层保留。

  **Exit / oracle**: 冷准备成功；错digest/任务/JSON冒充ONNX/缺initializer拒绝；无grant或Provider副作用，预算限制生效。

<a id="t004"></a>

- [ ] T004 [US1] Single Flight Refresh and Leases — ModelPreparationCache.cpp; tests/unit-tests/di-preparation.t.cpp

  **Batch / Depends**: B2 / T003 static。

  **Read / contract**: C-02 matrix/key/generation/lease。

  **Implementation and review**: 实现四种policy、独立waiter/job deadline、取消、generation CAS、预算/LRU和活动lease；补8线程及refresh交错fixture，真实调用缓存owner。

  **API revision**: C-06：PreparationHandle代表独立waiter；可靠completion迟注册仍一次交付，取消单waiter不能终止共享job。

  **Exit / oracle**: C-02全部反例；单waiter取消不影响其他人；Refresh失败旧对象可用；8并发仅一次fetch/inspect；TSan重复两次。

<a id="t005"></a>

- [ ] T005 [US2] Prepared Request Projection — PreparedModel.cpp; NativeInferenceClient.hpp/NativeInferenceClient.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B3 / B2E exit + Spec184 scoped dependency gate。

  **Read / contract**: NativeRequestPlanner.cpp, NativeRequestPreparation.cpp, C-01/C-03。

  **Implementation and review**: 将Package绑定既有client，保留旧五参数签名与逐请求expectedModel检查；映射Input/DataRef、默认与覆盖placement、generation/stream；不得缓存grant/candidate/plan。

  **API revision**: C-05：原生Input.text按adapter能力开放，不引入Python tokenizer；DataRef保留完整受保护引用，Provider继续取得/解密。

  **Exit / oracle**: 2次request共用Package且ID/授权独立；hot-cache revoke拒绝；repository错digest/size拒绝；invalid placement不能产生可执行Selection。

<a id="t006"></a>

- [ ] T006 [US2] Handle Deadlines Events and Cancellation — PreparedModel.hpp/PreparedModel.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B3 / T005 static。

  **Read / contract**: NativeInferenceHandle::result/observe/cancel; C-01 handle compatibility。

  **Implementation and review**: 实现RequestHandle wrapper及lease，wait双重载；逐项对照历史回放/观察者异常/慢消费者行为；记录有界队列现状和支持能力，不发明全局ASSEMBLING。

  **API revision**: C-05/C-06：统一Result/DiError/status；可退订CompletionSubscription、原生onCompletion与EventReader next/nextAsync、单游标、有界队列和STREAM_GAP独立于best-effort observe；覆盖慢读者、溢出、迟订阅和取消竞争。

  **Exit / oracle**: 局部wait超时后可取成功；deadline终态不可重复；cancel/complete/close交错；观察者不成为提交oracle；ASan/UBSan无抑制。

<a id="t007"></a>

- [ ] T007 [US2] Prepared Conversations and Committed Checkpoints — Conversation.hpp/Conversation.cpp; tests/integration-tests/di-prepared-conversation.t.cpp

  **Batch / Depends**: B4 / B3 exit。

  **Read / contract**: NativeConversationCoordinator/NativeConversationJournal; C-03。

  **Implementation and review**: 接入openConversation、request、checkpoint、close；Package/模型/任务/tokenizer绑定；单会话turn串行；由coordinator生成parent/role map，保留durableCommitGate。

  **API revision**: C-03：公开opaque ConversationCheckpoint，避免应用传receipt/commit owner；恢复仍校验安全绑定。

  **Exit / oracle**: 两轮真实native结果；stream final未commit时无新checkpoint；并发turn拒绝；不支持adapter及错模型checkpoint拒绝。

<a id="t008"></a>

- [ ] T008 [US2] Conversation Recovery Replacement and Export — Conversation.cpp; tests/integration-tests/di-prepared-conversation.t.cpp

  **Batch / Depends**: B4 / T007 static。

  **Read / contract**: Spec184 durable/export evidence; existing checkpoint export helper; C-03。

  **Implementation and review**: 复用原子private export与restore；配置恢复/替换接同一coordinator；覆盖成功提交后取消、Provider替换和Runtime重开；禁止第二套journal。

  **Exit / oracle**: 恢复后parent/hash chain一致；replacement attempt隔离；export失败旧文件保留；durable成功不降级；ASan/UBSan同selector两次。

<a id="t009"></a>

- [ ] T009 [US3] Provider Facade and Authenticated Assembly — Provider.hpp/Provider.cpp; NativeInferenceProvider.cpp; examples/DI_NativeProviderExecutable.cpp; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B5 / B4 exit。

  **Read / contract**: NativeProviderHandler/NativeRunnerPreparation/NativeCanonicalOnnxAssembler; C-03。

  **Implementation and review**: 从executable提取复用组合owner；serve/registration/stop/drain绑定既有handler；保留认证前后guard；将可观测source_fetch/assembly/runner_created counters接到生产边界。

  **API revision**: C-06：Provider::drainAsync及关闭竞争；独立ProviderConfig::fromFile/fromCommandLine及Runtime::open(ProviderConfig)，无User目录可单独serve；CLI复用C++ parser，非法字段拒绝。

  **Exit / oracle**: 无Selection时fetch/assembly=0；有效Selection执行；wrong provider/epoch/grant拒绝；registration关闭及stop清理不悬空。

<a id="t010"></a>

- [ ] T010 [US3] Protected Artifact and Runner Template Reuse — Provider.cpp; NativeRunnerPreparation.cpp; NativeProtectedArtifactStore.cpp; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B5 / T009 static。

  **Read / contract**: C-03 Provider cache layers。

  **Implementation and review**: 增加受限artifact/template cache及backend支持矩阵；key绑定role/recipe/ABI/device/security；per-request mutable runner与plaintext lease；命中仍重验Selection/grant。

  **Exit / oracle**: 相同artifact避免重复assembly；不同ABI/role/epoch不误命中；KV不串请求；撤销拒绝；drain后lease=0；ASan/UBSan两次。

<a id="t011"></a>

- [ ] T011 [US4] Native Caller Migration and Compatibility Registry — examples/DI_NativeRequester.cpp; examples/wscript; tests/integration-tests/di-prepared-compatibility.t.cpp; contracts/caller-matrix.md

  **Batch / Depends**: B6 / B5 exit。

  **Read / contract**: actual NativeInferenceClient callers, Spec184 caller-matrix, C-04。

  **Implementation and review**: 盘点维护调用方并按shared backend分组；C++ requester/provider使用Runtime/PreparedModel；保持旧签名对照与显式route日志；安装头消费示例，建立symbol→TU→target映射。

  **API revision**: C-06：安装prefix外部consumer仅包含api.hpp/provider.hpp；交付同步/异步prepare、unary/stream、conversation/recovery/replacement及Provider示例，不导入Python。

  **Exit / oracle**: 新旧结果/失败语义对照；C++真实进程unary/stream先通过；matrix无漏项；nm/readelf与安装消费链接确认。

<a id="t012"></a>

- [ ] T012 [US4] Thin Python Prepared Model Facade — pythonWrapper/src/ndnsf/di_bindings.cpp; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py; tests/python/test_spec185_prepared_model.py; contracts/caller-matrix.md

  **Batch / Depends**: B8 / B7 C++ qualification exit。

  **Read / contract**: existing binding declarations and maintained APPClient paths; C-01/C-04。

  **Implementation and review**: 在现有绑定模块导出同一native对象；prepare/wait/drain释放GIL，observe正确获取GIL并保持owner；迁移维护Python用户路径，兼容shim保留/删除逐项记录，不在Python重做planning。

  **API revision**: C-05/C-06：固定导出、timeout_s、结构化错误、上下文退出与asyncio adapter；桥接C++ completion/nextAsync，禁止Python状态机或线程补齐缺失native能力。

  **Exit / oracle**: Python输入/异常/事件映射和寿命通过；native断言仍C++；无静默legacy fallback；若实际模块文件不同先更新精确caller映射。

<a id="t013"></a>

- [ ] T013 [US4] Current Candidate Process Qualification — tests/integration-tests/di-prepared-process.t.cpp; tests/wscript; evidence/b7-cpp-qualification.md

  **Batch / Depends**: B7 / B6 exit。

  **Read / contract**: C-04 full five-lane convergence and dynamic matrix。

  **Implementation and review**: 先审查完整调用链/source closure，再构建同树candidate并刷新binary receipt；独立authority/requester/provider完成unary/stream/continuation/recovery/replacement/cancel/revoke/cleanup，C++ oracle判定。

  **API revision**: C-06：T012之前完成全部native矩阵、安装消费和ELF/子进程no-Python闭包；不可用Python PASS补缺，SC-005包装行留后批。

  **Exit / oracle**: SC-001至SC-007的原生部分逐条证据；SC-005 Python部分留T012；no-Python ELF/进程依赖；不同安全域和cache命中反例；无startup/collector错误冒充协议结果。

<a id="t014"></a>

- [ ] T014 [US4] Design API and Scoped Handoff — Design/; specs/185-prepared-model-runtime/; docs/architecture.md

  **Batch / Depends**: B9 / T012 acceptance。

  **Read / contract**: Design/MANAGEMENT.md, C-04, Spec184 outstanding registry。

  **Implementation and review**: 更新实际当前API/中文契约/源码摘要/三类图/双PDF；核对caller退出和每FR/SC证据；记录184仍未完成外部资格，提交明确candidate/source交付入口。

  **API revision**: C-05/C-06：同步六层API exposure manifest、全声明索引及独立C++指南；原生与包装验收分别记录。

  **Exit / oracle**: 所有本Spec任务完整验收且链接可追溯；当前设计不混入planned；双PDF身份/排版通过；184不被自动勾选。

<a id="t015"></a>

- [ ] T015 [US4] Installed C++ API and ABI Closure — root wscript; ndnsf-distributed-inference.pc.in; NDNSF-DistributedInference/cpp/ndnsf-di/adapters/onnx/OnnxRuntimeModelRunner.hpp; tests/installed-api/

  **Batch / Depends**: B0 / none。

  **Read / contract**: C-05/C-06；api-review U10/U11/U12；实际头安装规则和宏条件布局。

  **Implementation and review**: 建立contracts/api-exposure.json逐符号/头分层清单；application/provider umbrella与advanced/internal边界；修复安装include闭包，保留合法旧consumer兼容；ONNX公开类固定PImpl或退到非公开头。开始时核对实际源码路径。

  **Exit / oracle**: 每个安装公共头单独包含；外部consumer仅用安装prefix/pkg-config编译链接及构造析构；ONNX enabled/disabled各自同配置安装消费，normal及ASan构造/析构通过。Spec185InstalledApi记录include/link/运行边界，不以--help代替。

<a id="t016"></a>

- [ ] T016 [US4] Extension Registration and Cooperative Control — NativePlanning.hpp/NativePlanning.cpp; NativeModelRunner.hpp/NativeModelRunner.cpp; NativeRequestPlanner.cpp; tests/unit-tests/di-extension-contract.t.cpp

  **Batch / Depends**: B2E / B2 exit。

  **Read / contract**: C-05 Extension Lifecycle；api-review U13/U14/U15；实际registry、strategy及runner调用方。

  **Implementation and review**: startup builder后freeze，duplicate拒绝、显式replace仅限freeze前；runner实例并发所有权；新strategy/control端口带deadline/cancel。旧非协作端口留高级兼容，普通Runtime拒绝不满足控制契约的插件，不声称强制抢占任意回调。

  **Exit / oracle**: Spec185ExtensionRegistry覆盖重复/冻结/替换、并发lookup、协作超时/cancel、旧插件拒绝及runner隔离；TSan同矩阵两次；过期不能发布Selection。

## Fragmentation Review

16任务按11个可观察批次组织；B0安装/ABI、B2E扩展边界有独立出口；B1–B5成对组合，B6–B9分别迁移、完整C++资格、Python包装与文档。任务卡保留ID顺序，执行以顶部registry及依赖为准。
Runtime与cache不同owner、request与conversation不同持久语义、Provider与Python不同安全/验收边界，因此不合并。
T013是真实集成验收，T014是源码/API/资格交付，不替代前面的行为测试。
