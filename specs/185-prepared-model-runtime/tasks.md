# Tasks: Prepared Model Runtime

**Status**: PLANNED | **Date**: 2026-09-12
**Input**: [spec](spec.md) · [plan](plan.md) · [C-01](contracts/public-api.md) · [C-02](contracts/preparation.md) · [C-03](contracts/execution.md) · [C-04](contracts/validation.md) · [C-05](contracts/api-usability.md) · [C-06](contracts/cpp-first.md) · [C-07](contracts/api-catalog.md) · [C-08](contracts/code-design.md) · [C-09](contracts/core-app-boundary.md)

## Execution Progress

**Progress Timestamp**: `YYYY-MM-DD HH:mm ±HH:MM`，项目时区`America/Chicago`；Updated为该行最后修订时间，不是完成时间。
本表于`2026-09-12 16:24 -05:00`升级时间格式，原18行仅记录`2026-09-12`，精确历史事件时间UNKNOWN；本次统一时间仅表示格式迁移。后续只更新状态、依赖、证据或剩余项实际变化的行，规则见[task progress](../../skills/speckit-code-design/references/task-progress.md#progress-timestamp)。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T015 Installed C++ API and ABI Closure](#t015) | PASS | none | B0 closed; [b0-installed-api](evidence/b0-installed-api.md) covers v17 STATIC_PASS, four C++ consumers, 67 headers, negative gate, ABI/ldd/hash | 2026-09-12 16:24 -05:00 |
| [T017 Core Operation Runtime and Channels](#t017) | PASS | B0 exit | B0C closed; [b0c-core-operation](evidence/b0c-core-operation.md), PO-C1,C2 C++/TSan/installed-consumer PASS | 2026-09-12 16:24 -05:00 |
| [T018 DI Delegation to Core Operations](#t018) | PASS | T017 static | B0C closed; [b0c-core-operation](evidence/b0c-core-operation.md), PO-C3,C4 C++ real-provider/regression PASS | 2026-09-12 16:24 -05:00 |
| [T001 Runtime Configuration and Export](#t001) | PASS | B0C exit | B1 closed; [b1-runtime](evidence/b1-runtime.md) static/compile-link/runtime PASS; later request path remains open | 2026-09-12 16:24 -05:00 |
| [T002 Runtime Shutdown and Child Ownership](#t002) | PASS | T001 static | B1 closed; [b1-runtime](evidence/b1-runtime.md) normal/TSan/installed C++ lifecycle PASS; owner-thread public path remains unobserved | 2026-09-12 16:24 -05:00 |
| [T016 Extension Registration and Cooperative Control](#t016) | PASS | B1 exit | B2E closed; [b2e-extensions](evidence/b2e-extensions.md) static/compile-link/runtime/TSan/installed C++ PASS; full packaging remains unobserved | 2026-09-12 16:24 -05:00 |
| [T003 Verified Package Preparation](#t003) | PASS | B2E exit | B2 closed; [b2-preparation](evidence/b2-preparation.md) covers static/combination review, normal/TSan C++ preparation and affected Runtime/Core suites | 2026-09-12 21:07 -05:00 |
| [T004 Single Flight Refresh and Leases](#t004) | PASS | T003 static | B2 closed; [b2-preparation](evidence/b2-preparation.md) covers static/combination review, normal/TSan C++ preparation and lease/refresh/concurrency cases | 2026-09-12 21:07 -05:00 |
| [T005 Prepared Request Projection](#t005) | PASS | B2 exit + Spec184 scoped dependency gate | B3 closed; [b3-request](evidence/b3-request.md) covers static/combination review, normal and ASan/UBSan C++ prepared-request selectors (12/12 x 2 each) | 2026-09-13 07:50 -05:00 |
| [T006 Handle Deadlines Events and Cancellation](#t006) | PASS | T005 static | B3 closed; [b3-request](evidence/b3-request.md) covers handle/deadline/event/cancel/drain cases, normal and ASan/UBSan C++ extension/request selectors (10/10 and 12/12 x 2 each) | 2026-09-13 07:50 -05:00 |
| [T007 Prepared Conversations and Committed Checkpoints](#t007) | NOT_STARTED | B3 exit | B4 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T008 Conversation Recovery Replacement and Export](#t008) | NOT_STARTED | T007 static | B4 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T009 Provider Facade and Authenticated Assembly](#t009) | NOT_STARTED | B4 exit | B5 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T010 Protected Artifact and Runner Template Reuse](#t010) | NOT_STARTED | T009 static | B5 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T011 Native Caller Migration and Compatibility Registry](#t011) | NOT_STARTED | B5 exit | B6 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T013 Current Candidate Process Qualification](#t013) | NOT_STARTED | B6 exit | B7 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T012 Thin Python Prepared Model Facade](#t012) | NOT_STARTED | B7 C++ qualification exit | B8 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |
| [T014 Design API and Scoped Handoff](#t014) | NOT_STARTED | T012 acceptance | B9 planned; implementation/build/runtime NOT_RUN | 2026-09-12 16:24 -05:00 |

## Current Checkpoint

2026-09-13 07:50 -05:00 B3 closed：T005→T006 完成逐任务官方 `review-agent` 静态门、受影响复审及最终组合门；最终组合快照 v6 的 base/diff/path 身份见 [b3-request](evidence/b3-request.md)。normal `Spec185PreparedRequest` 12/12 与 `Spec185ExtensionRegistry` 10/10 各顺序重复两次；独立 ASan/UBSan + LSan 两个 selector 同样各重复两次，均 `rc=0`、`*** No errors detected` 且无 LSan/UBSan 报告。批次唯一证据为 [b3-request](evidence/b3-request.md)；保留并发资源干扰及早期 LSan/deadline 失败边界。B3 未运行 TSan、B4–B9、Python、跨进程资格、SIF/Tiger/MiniNDN；下一依赖满足批次为 T007/B4，Spec185 整体仍为 `PLANNED`。

2026-09-12 23:03 -05:00 Build policy documentation：按用户要求将所有机器后续SIF构建版本固定为Apptainer1.5.3；同步Tiger/packaging说明、交付入口、版本化/个人操作skill及本机AGENTS。文档diff/版本规则一致性检查通过；无API/产品变化、无新构建或Tiger运行，任务状态/依赖不变。下一步实验机按[SIF规则](../../Experiments/TigerCluster/docs/sif-build.md#apptainer-version-policy)核对环境。
2026-09-12 22:59 -05:00 Host tooling：用户要求本机Apptainer升级1.5.3，安装、默认/兼容/root入口和最小SIF构建执行通过；见[运维记录](../../Experiments/TigerCluster/docs/apptainer-153-upgrade-20260912.md)。不改变185任务状态、依赖或验收；计算节点版本由实验机在实际作业核验，此记录不是185产品完成证据。

2026-09-12 21:07 -05:00 B2 closed：T003→T004 完成逐任务官方 review-agent 静态门、B2组合门及批末共享验收。普通 `Spec185Preparation` 14/14、`Spec185Runtime` 10/10、`Spec185CoreOperation` 35/35 通过；独立 clang/TSan 三套件各重复两次共6次均 `rc=0` 且 `*** No errors detected`。证据见[b2-preparation](evidence/b2-preparation.md)，失败边界及修复见 `docs/failure-log.md`。准备链未运行 Python、Provider、会话、跨进程资格、SIF/Tiger；下一依赖满足任务为T005/B3。

2026-09-12 17:00 -05:00 Dependency-scoped gate revision：门禁仅阻塞依赖工作；无依赖、文件边界清晰且前置满足的任务可由主代理在子代理只读审查固定快照期间继续。执行细则见[批次执行表](batch-execution.md)，验证见[调度修订](evidence/dependency-scoped-dispatch-20260912.md)。现有任务状态、Depends、Updated和验收不变；本轮没有启动并行产品实现。下一步执行者先登记独立任务/子任务边界，无合格工作则等待，不绕过T003→T004等硬依赖。

2026-09-12 16:24 -05:00 Progress timestamp revision：18行Updated升级为分钟及UTC offset；同步Spec Kit规则、模板和本机安装入口。状态/勾选/验收证据不变，历史只有日期的checkpoint原样保留；本次时间不是历史完成时间。见[时间规则验证](evidence/progress-timestamps-20260912.md)。下一产品任务仍按当前registry及依赖选择。

2026-09-12 B2E closed：T016 已完成 v9/v10b/v11/v12 官方 review-agent 静态门及 B2E 组合审查；normal `Spec185ExtensionRegistry` 10/10、clang/TSan 重复两次各10/10、installed-prefix C++ extension consumer 通过。失败的 fixture 身份与系统工具链边界均已保留并修复，详见[b2e-extensions](evidence/b2e-extensions.md)；完整 Waf packaging、准备/请求/会话/Provider/完整资格/Python/文档仍未完成。下一依赖满足任务为T003/B2。
2026-09-12 B1 closed：T001/T002 已完成官方 review-agent 静态门（T001 v7、T002 v4）及 B1 组合审查；正常 DI 7/7、Core 34/34，DI/Core TSan 各按要求重复通过，安装前缀 C++ Runtime consumer 通过。证据见[b1-runtime](evidence/b1-runtime.md)。全树安装曾在无关 spec181 链接和 Python editable hook 边界停止，未计入B1产品失败；准备/请求/会话/Provider/完整资格/Python/文档仍未完成。下一依赖满足任务为T016/B2E。
2026-09-12 B0C closed：T017/T018 已完成 v29 官方 review-agent 静态门及组合审查；正常 Core selector 33、DI selector 5、Spec170 回归59、TSan Core重复2次、Core-only staged installed consumer均通过。证据见[b0c-core-operation](evidence/b0c-core-operation.md)；未观测项为全树安装、Python绑定、Runtime及后续准备/请求/会话/Provider/资格批次。下一依赖满足任务为T001/B1。
2026-09-12 B0/T015 closed：安装 API/ABI 证据[b0-installed-api](evidence/b0-installed-api.md)记录 v17 STATIC_PASS、四配置 C++ consumer、67 独立头和 DI/SVS ldd/hash；下一依赖满足任务为T017/B0C。其余17任务保持NOT_STARTED；已补[批次执行表](batch-execution.md)。
每任务编码后review-agent静态门→同批继续→整批组合审查→共享构建/定向测试；不逐小修改编译，也不拖到全Spec末尾首次测试。
本轮为执行计划整理，未修改API/产品设计；验证见[evidence](evidence/batch-execution-20260912.md)。下一步T015/B0。

2026-09-12 Core/App修订：源码确认四消息/协作/流/scoped registration已有Core实现，但DI仍自持通用executor和等待状态。
新增[C-09](contracts/core-app-boundary.md)与T017/T018，18任务12批，全部NOT_STARTED；当前顺序B0→B0C→B1→B2E→B2及其余原序。
本轮设计覆盖此前“新公开类都在DI”的表述；DI保留领域包装，通用实现下移Core。证据见[boundary audit](evidence/core-boundary-20260912.md)。
以下16任务/11批记录为上一文档checkpoint历史，不能作为当前执行队列；当前执行队列为18任务/12批。

2026-09-12：核对现有skill确有Class/Function/Field契约，但185原任务缺内部实现绑定；已补[C-08](contracts/code-design.md)，逐任务Design binding覆盖类/文件delta、字段、关键函数、流程和PO。
新增shared skill开工前设计检查，更新plan/tasks模板及本机入口/个人安装副本。历史记录为16任务11批；当前为18任务12批，全部NOT_STARTED。
T016提供合作splitter，前移到B1后/T003前；实际顺序T015→T001/T002→T016→T003–T011→T013→T012→T014。
本轮[证据](evidence/implementation-design-20260912.md)；设计/技能验证不计产品完成，生产源码及native构建测试未运行。
下一实现单元T015；保持Spec184资格和既有Design 54文件漂移边界。

## Shared Task Rules

调度使用[Dependency-Scoped Dispatch](../../skills/speckit-code-design/references/pre-test-static-review.md#dependency-scoped-dispatch)：依赖工作等待静态通过，无依赖且文件边界清晰的就绪工作可继续；固定快照审查、失败依赖闭包和批末组合门均保留。

路径缩写 `Runtime.cpp` 等未带前缀的DI文件均位于
`NDNSF-DistributedInference/cpp/ndnsf-di/`；tests/examples/pythonWrapper/Design路径从repo root解析。
原生行为任务先编写相应C++反例与fixture，再实现完整行为并用官方review-agent只读审查全diff及五lane。
同批任务静态通过、测试尚未运行时状态PARTIAL，不能勾选。B0C、B1、B2、B3、B4、B5在全部成员静态门和组合门通过后共享构建/测试；
B0、B2E、B6–B9是单任务批次，各自达到出口即验证。完整批次/五lane/构建复测范围见[执行表](batch-execution.md)。T012只写binding断言，T014只做文档交付。
跨批验收依赖必须实际通过；T012还要求T013完整C++ qualification出口，不接受仅静态接线。
成员证据采用C-04同一批记录模板，不创建第二份进度权威。
C-07每行是T015 exposure、T011 C++消费、T012绑定和T014文档的共同检查项；没有实现/证据不能关闭对应任务。
所有新生产公共方法补C-01英文Doxygen义务；涉及private owner/提交点补说明原因的英文注释。
目标路径须在task开始用CodeGraph/rg定位，若历史路径变动先更新本卡与symbol map，不能在未知文件下另建重复实现。

## Implementation Units

<a id="t015"></a>

- [x] T015 [US4] Installed C++ API and ABI Closure — root wscript; libndn-service-framework.pc.in; NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in; NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp; tests/installed-api/

  **Batch / Depends**: B0 / none。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD10 / FN09 / PO09；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-05/C-06；api-review U10/U11/U12；实际头安装规则和宏条件布局。

  **Implementation and review**: 建立contracts/api-exposure.json逐符号/头分层清单；application/provider umbrella与advanced/internal边界；修复安装include闭包，保留合法旧consumer兼容；ONNX按C-08 FN09固定无条件PImpl及disabled分支完整Impl。开始时核对实际源码路径。

  **Exit / oracle**: 每个安装公共头单独包含；外部consumer仅用安装prefix/pkg-config编译链接及构造析构；ONNX enabled/disabled各自同配置安装消费，normal及ASan构造/析构通过。Spec185InstalledApi记录include/link/运行边界，不以--help代替。已通过，见[evidence/b0-installed-api.md](evidence/b0-installed-api.md)。

<a id="t017"></a>

- [x] T017 [US1] Core Operation Runtime and Channels — ndn-service-framework/OperationRuntime.hpp/.cpp; ndn-service-framework/OperationState.hpp; tests/unit-tests/core-operation-runtime.t.cpp; tests/installed-api/core-operation-consumer.cpp; wscript; tests/wscript

  **Batch / Depends**: B0C / B0 exit。

  **Design binding**: [C-09](contracts/core-app-boundary.md#concrete-design-binding) CB01,CB02 / PO-C1,C2；文件、字段owner及关键签名按契约，状态PLANNED。

  **Implementation and review**: 提取通用调度/ticket/close/drain、完成等待/退订/可靠reader；复用Core已有stream传输，不导入DI领域类型。逐任务review-agent只读静态门。

  **Exit / oracle**: Core-only安装消费者无DI/Python/ONNX依赖；PO-C1全部C++竞态/生命周期反例通过，TSan重复两次；批末与T018共同验证。已通过，见[evidence/b0c-core-operation.md](evidence/b0c-core-operation.md)。

<a id="t018"></a>

- [x] T018 [US2] DI Delegation to Core Operations — NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp/.cpp; tests/integration-tests/di-core-operation.t.cpp; tests/wscript

  **Batch / Depends**: B0C / T017 static；启动前核对Spec184同树生产链及并行源码变化。

  **Design binding**: [C-09](contracts/core-app-boundary.md#concrete-design-binding) CB03,CB04 / PO-C3,C4；旧签名兼容、领域完成时机不变。

  **Implementation and review**: 删除DI私有executor与重复通用等待状态，桥接Core State；保留模型/attempt/会话语义，复用BeginCollaboration/CommitCollaborationPlan/CancelCollaboration及scoped registration。静态审查不得把stream final当durable完成。

  **Coverage lanes**: production entry/callers→NativeInferenceClient/Core-only consumer；implementation and wire→C-09 CB01–CB04及旧Core协议；test/harness/oracle→PO-C1–C3及C++fixture；build/source closure→wscript/安装消费者/PO-C2,C4；migration/evidence→旧API、重复owner退出、PO-C4及批次证据。安全/错误/并发穿过五lane核对。

  **Exit / oracle**: 两任务静态门与组合流程审查后运行Spec185CoreOperation、Spec185DiCoreOperation及受影响Core流/协作/注册回归；记录review-agent路径/SHA、候选与selector、static/compile-link/runtime-test/unobserved漏检复盘。C++断言闭合且依赖零反向边才CLOSED_FOR_VALIDATION，否则保留OPEN_FOR_NEXT_BATCH及具体触发条件；已通过，结果见[evidence/b0c-core-operation.md](evidence/b0c-core-operation.md)。

<a id="t001"></a>

- [x] T001 [US1] Runtime Configuration and Export — Runtime.hpp/Runtime.cpp; root wscript; tests/unit-tests/di-runtime.t.cpp

  **Batch / Depends**: B1 / B0C exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD01 / F01–F04 / FN01 / FLOW01 / PO01；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeInferenceClient constructors, NativeRequestRuntime, examples/DI_NativeRequester.cpp; C-01。

  **Implementation and review**: 提取现有operator配置组合为Runtime::open；校验profile、trust、所有资源上限；新增公开头安装/target map。RuntimeConfig.nativeConfigPath沿用当前native requester-v1配置；单用户default规则见C-01；taskContract独立绑定，不能从catalog猜测。

  **API revision**: C-05/C-06：启动冻结model key注册与精确配置；稳定application头不泄露advanced/native类型。

  **Exit / oracle**: 有效配置可创建User；非default profile拒绝、错误trust/零预算/初始化中途失败无残留；公开头消费链接成功。

  **Completion**: B1 `PASS`; static v7, normal/TSan C++ selectors, and installed-prefix consumer are recorded in [evidence/b1-runtime.md](evidence/b1-runtime.md).

<a id="t002"></a>

- [x] T002 [US1] Runtime Shutdown and Child Ownership — Runtime.cpp; tests/unit-tests/di-runtime.t.cpp

  **Batch / Depends**: B1 / T001 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD01,CD04 / F01,F02,F10 / FN01,FN04 / FLOW07 / PO01,PO04；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: SerialRequestExecutor, NativeInferenceClient::close, C-01 lifetime。

  **Implementation and review**: 按C-06实现原生drainAsync及可退订token；接入owner registry、close/drain和失败逆序清理；weak callback断环；补prepare/wait/drain的owner线程拒绝路径和最后owner释放流程。

  **Lifecycle completeness**: C-07 Runtime外壳析构即close；drainAsync屏障排除自身通知，保留安全State及join；测试子对象仍在/已释放和owner线程最后释放。

  **Exit / oracle**: close幂等、回调中close无自join、drain超时可重试；外部Face/IO fixture显式寿命；TSan同矩阵两次通过。

  **Completion**: B1 `PASS`; static v4 after repair, normal/TSan lifecycle runs, and C++ installed consumer are recorded in [evidence/b1-runtime.md](evidence/b1-runtime.md). Public owner-thread prepare/request paths remain deferred to later tasks.

<a id="t016"></a>

- [x] T016 [US4] Extension Registration and Cooperative Control — NativePlanning.hpp/NativePlanning.cpp; NativeModelRunner.hpp/NativeModelRunner.cpp; NativeRequestPlanner.cpp; tests/unit-tests/di-extension-contract.t.cpp

  **Batch / Depends**: B2E / B1 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD08,CD09 / F16 / FN08 / FLOW03 / PO08；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-05 Extension Lifecycle；api-review U13/U14/U15；实际registry、strategy及runner调用方。

  **Implementation and review**: startup builder后freeze，duplicate拒绝、显式replace仅限freeze前；runner实例并发所有权；新strategy/control端口带deadline/cancel。旧非协作端口留高级兼容，普通Runtime拒绝不满足控制契约的插件，不声称强制抢占任意回调。

  **Exit / oracle**: Spec185ExtensionRegistry覆盖重复/冻结/替换、并发lookup、协作超时/cancel、旧插件拒绝及runner隔离；TSan同矩阵两次；过期不能发布Selection。

  **Completion**: B2E `PASS`; v12 static re-review and prior composition pass, normal/TSan C++ selectors, and installed-prefix extension consumer are recorded in [evidence/b2e-extensions.md](evidence/b2e-extensions.md). Full Waf packaging and later preparation/request/Provider/Python exits remain unobserved.

<a id="t003"></a>

- [x] T003 [US1] Verified Package Preparation — PreparedModel.hpp/PreparedModel.cpp; ModelPreparationCache.hpp/ModelPreparationCache.cpp; tests/unit-tests/di-preparation.t.cpp

  **Batch / Depends**: B2 / B2E exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD02 / F04–F09 / FN02 / FLOW02 / PO02；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeRequestCatalog/NativeCanonicalPreparationCatalog/NativeRequestPreparation; C-02。

  **Implementation and review**: 实现User::prepare的owned source路径及既有受保护fetch接线；校验配置/双graph/initializer/adapter；将冻结catalog收进Package，构造后才发布。补独立内容oracle和生产解析计数。

  **API revision**: C-05/C-06：普通prepare("default")与原生prepareAsync共享唯一owner；只读capabilities/输入输出schema；高级PrepareRequest另层保留。

  **Exit / oracle**: 冷准备成功；错digest/任务/JSON冒充ONNX/缺initializer拒绝；无grant或Provider副作用，预算限制生效。

  **Completion**: B2 `PASS`; static/combination review, normal and repeated clang/TSan C++ selectors, independent graph oracle, and Runtime/Core affected suites are recorded in [evidence/b2-preparation.md](evidence/b2-preparation.md). Request, conversation, Provider, Python and full qualification remain deferred to later batches.

<a id="t004"></a>

- [x] T004 [US1] Single Flight Refresh and Leases — ModelPreparationCache.cpp; tests/unit-tests/di-preparation.t.cpp

  **Batch / Depends**: B2 / T003 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD02,CD04 / F06–F10 / FN02,FN04 / FLOW02 / PO02,PO04；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-02 matrix/key/generation/lease。

  **Implementation and review**: 实现四种policy、独立waiter/job deadline、取消、generation CAS、预算/LRU和活动lease；补8线程及refresh交错fixture，真实调用缓存owner。

  **API revision**: C-06：PreparationHandle代表独立waiter；可靠completion迟注册仍一次交付，取消单waiter不能终止共享job。

  **Lifecycle completeness**: C-07 PreparationHandle最后副本释放取消该waiter；resultAsync仅取消等待；移除普通PrepareOptions取消回调，native handle统一取消。

  **Exit / oracle**: C-02全部反例；单waiter取消不影响其他人；Refresh失败旧对象可用；8并发仅一次fetch/inspect；TSan重复两次。

  **Completion**: B2 `PASS`; single-flight, refresh generation, lease/LRU/budget, waiter cancellation/deadline and exactly-once completion cases passed in normal and repeated clang/TSan C++ selectors. Full cross-process and Python qualification remain unobserved; see [evidence/b2-preparation.md](evidence/b2-preparation.md).

<a id="t005"></a>

- [x] T005 [US2] Prepared Request Projection — PreparedModel.cpp; NativeInferenceClient.hpp/NativeInferenceClient.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B3 / B2 exit + Spec184 scoped dependency gate。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD03 / F03,F05,F16 / FN03,FN08 / FLOW03 / PO03,PO08；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeRequestPlanner.cpp, NativeRequestPreparation.cpp, C-01/C-03。

  **Implementation and review**: 将Package绑定既有client，保留旧五参数签名与逐请求expectedModel检查；映射Input/DataRef、默认与覆盖placement、generation/stream；不得缓存grant/candidate/plan。

  **API revision**: C-05：原生Input.text按adapter能力开放，不引入Python tokenizer；DataRef保留完整受保护引用，Provider继续取得/解密。

  **Exit / oracle**: `PASS` in B3；2次request共用Package且ID/授权独立；hot-cache revoke拒绝；repository错digest/size拒绝；invalid placement不能产生可执行Selection；详见[evidence/b3-request.md](evidence/b3-request.md)。

<a id="t006"></a>

- [x] T006 [US2] Handle Deadlines Events and Cancellation — PreparedModel.hpp/PreparedModel.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B3 / T005 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD03,CD04 / F10–F12 / FN04 / FLOW04,FLOW07 / PO04；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeInferenceHandle::result/observe/cancel; C-01 handle compatibility。

  **Implementation and review**: 实现RequestHandle wrapper及lease，wait双重载；逐项对照历史回放/观察者异常/慢消费者行为；记录有界队列现状和支持能力，不发明全局ASSEMBLING。

  **API revision**: C-05/C-06：统一Result/DiError/status；可退订Subscription、原生onCompletion与EventReader next/nextAsync、单游标、有界队列和STREAM_GAP独立于best-effort observe；覆盖慢读者、溢出、迟订阅和取消竞争。

  **Lifecycle completeness**: C-07 observe/nextAsync返回Subscription；验证退订不吞事件、错误流不伪装EOF、READ_IN_PROGRESS、64订阅额度回收及moved-from。

  **Exit / oracle**: `PASS` in B3；局部wait超时后可取成功；deadline终态不可重复；cancel/complete/close交错；观察者不成为提交oracle；normal 与 ASan/UBSan + LSan 无抑制通过；详见[evidence/b3-request.md](evidence/b3-request.md)。

<a id="t007"></a>

- [ ] T007 [US2] Prepared Conversations and Committed Checkpoints — Conversation.hpp/Conversation.cpp; tests/integration-tests/di-prepared-conversation.t.cpp

  **Batch / Depends**: B4 / B3 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD05 / F13 / FN05 / FLOW05 / PO05；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeConversationCoordinator/NativeConversationJournal; C-03。

  **Implementation and review**: 接入openConversation、request、checkpoint、close；Package/模型/任务/tokenizer绑定；单会话turn串行；由coordinator生成parent/role map，保留durableCommitGate。

  **API revision**: C-03：公开opaque ConversationCheckpoint，避免应用传receipt/commit owner；恢复仍校验安全绑定。

  **Exit / oracle**: 两轮真实native结果；stream final未commit时无新checkpoint；并发turn拒绝；不支持adapter及错模型checkpoint拒绝。

<a id="t008"></a>

- [ ] T008 [US2] Conversation Recovery Replacement and Export — Conversation.cpp; tests/integration-tests/di-prepared-conversation.t.cpp

  **Batch / Depends**: B4 / T007 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD05 / F13 / FN05 / FLOW05 / PO05；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: Spec184 durable/export evidence; existing checkpoint export helper; C-03。

  **Implementation and review**: 复用原子private export与restore；配置恢复/替换接同一coordinator；覆盖成功提交后取消、Provider替换和Runtime重开；禁止第二套journal。

  **Exit / oracle**: 恢复后parent/hash chain一致；replacement attempt隔离；export失败旧文件保留；durable成功不降级；ASan/UBSan同selector两次。

<a id="t009"></a>

- [ ] T009 [US3] Provider Facade and Authenticated Assembly — Provider.hpp/Provider.cpp; NativeInferenceProvider.cpp; examples/DI_NativeProviderExecutable.cpp; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B5 / B4 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD06 / F02,F14 / FN06 / FLOW06,FLOW07 / PO06；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeProviderHandler/NativeRunnerPreparation/NativeCanonicalOnnxAssembler; C-03。

  **Implementation and review**: 从executable提取复用组合owner；serve/registration/stop/drain绑定既有handler；保留认证前后guard；将可观测source_fetch/assembly/runner_created counters接到生产边界。

  **API revision**: C-06：Provider::drainAsync及关闭竞争；独立ProviderConfig::fromFile/fromCommandLine及Runtime::open(ProviderConfig)，无User目录可单独serve；CLI复用C++ parser，非法字段拒绝。

  **Lifecycle completeness**: C-07重复serve拒绝；registration析构停新接收、Provider handle析构不stop；测试Runtime.close与已接收工作收敛。

  **Exit / oracle**: 无Selection时fetch/assembly=0；有效Selection执行；wrong provider/epoch/grant拒绝；registration关闭及stop清理不悬空。

<a id="t010"></a>

- [ ] T010 [US3] Protected Artifact and Runner Template Reuse — Provider.cpp; NativeRunnerPreparation.cpp; NativeProtectedArtifactStore.cpp; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B5 / T009 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD07 / F15 / FN07 / FLOW06 / PO07；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-03 Provider cache layers。

  **Implementation and review**: 增加受限artifact/template cache及backend支持矩阵；key绑定role/recipe/ABI/device/security；per-request mutable runner与plaintext lease；命中仍重验Selection/grant。

  **Exit / oracle**: 相同artifact避免重复assembly；不同ABI/role/epoch不误命中；KV不串请求；撤销拒绝；drain后lease=0；ASan/UBSan两次。

<a id="t011"></a>

- [ ] T011 [US4] Native Caller Migration and Compatibility Registry — examples/DI_NativeRequester.cpp; examples/wscript; tests/integration-tests/di-prepared-compatibility.t.cpp; contracts/caller-matrix.md

  **Batch / Depends**: B6 / B5 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD10,CD11 / FN09,FN10 / FLOW01–FLOW07 / PO09,PO10；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: actual NativeInferenceClient callers, Spec184 caller-matrix, C-04。

  **Implementation and review**: 盘点维护调用方并按shared backend分组；C++ requester/provider使用Runtime/PreparedModel；保持旧签名对照与显式route日志；安装头消费示例，建立symbol→TU→target映射。

  **API revision**: C-06：安装prefix外部consumer仅包含api.hpp/provider.hpp；交付同步/异步prepare、unary/stream、conversation/recovery/replacement及Provider示例，不导入Python。

  **Lifecycle completeness**: C-07 A01–A64逐组完整外部C++例子与行为oracle；新增入口必须更新表，普通应用不得引用Native内部头。

  **Exit / oracle**: 新旧结果/失败语义对照；C++真实进程unary/stream先通过；matrix无漏项；nm/readelf与安装消费链接确认。

<a id="t013"></a>

- [ ] T013 [US4] Current Candidate Process Qualification — tests/integration-tests/di-prepared-process.t.cpp; tests/wscript; evidence/b7-cpp-qualification.md

  **Batch / Depends**: B7 / B6 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD11 / FN10 / FLOW01–FLOW07 / PO01–PO10（资格fixture）；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-04 full five-lane convergence and dynamic matrix。

  **Implementation and review**: 先审查完整调用链/source closure，再构建同树candidate并刷新binary receipt；独立authority/requester/provider完成unary/stream/continuation/recovery/replacement/cancel/revoke/cleanup，C++ oracle判定。

  **API revision**: C-06：T012之前完成全部native矩阵、安装消费和ELF/子进程no-Python闭包；不可用Python PASS补缺，SC-005包装行留后批。

  **Lifecycle completeness**: C-07生命周期矩阵全部原生反例先通过；Python随后仅验边界语义。

  **Exit / oracle**: SC-001至SC-008的原生部分逐条证据；SC-005 Python部分留T012；no-Python ELF/进程依赖；不同安全域和cache命中反例；无startup/collector错误冒充协议结果。

<a id="t012"></a>

- [ ] T012 [US4] Thin Python Prepared Model Facade — pythonWrapper/src/ndnsf/di_bindings.cpp; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py; NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py; tests/python/test_spec185_prepared_model.py; contracts/caller-matrix.md

  **Batch / Depends**: B8 / B7 C++ qualification exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD12 / F10 / FN10 / C-07 mappings / PO10；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: existing binding declarations and maintained APPClient paths; C-01/C-04。

  **Implementation and review**: 在现有绑定模块导出同一native对象；prepare/wait/drain释放GIL，observe正确获取GIL并保持owner；迁移维护Python用户路径，兼容shim保留/删除逐项记录，不在Python重做planning。

  **API revision**: C-05/C-06：固定导出、timeout_s、结构化错误、上下文退出与asyncio adapter；桥接C++ completion/nextAsync，禁止Python状态机或线程补齐缺失native能力。

  **Lifecycle completeness**: C-07直接pybind对象/便利方法逐项映射；start_prepare保留显式handle；async取消/GC/loop关闭/解释器退出反例；不引入Python领域owner。

  **Exit / oracle**: Python输入/异常/事件映射和寿命通过；native断言仍C++；无静默legacy fallback；若实际模块文件不同先更新精确caller映射。

<a id="t014"></a>

- [ ] T014 [US4] Design API and Scoped Handoff — Design/; specs/185-prepared-model-runtime/; docs/architecture.md

  **Batch / Depends**: B9 / T012 acceptance。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) N/A生产类改动；实际Design/API/PDF与导出/证据交付，见FN10；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: Design/MANAGEMENT.md, C-04, Spec184 outstanding registry。

  **Implementation and review**: 更新实际当前API/中文契约/源码摘要/三类图/双PDF；核对caller退出和每FR/SC证据；记录184仍未完成外部资格，提交明确candidate/source交付入口。

  **API revision**: C-05/C-06：同步六层API exposure manifest、全声明索引及独立C++指南；原生与包装验收分别记录。

  **Lifecycle completeness**: 核对C-07全表、exposure、安装头和Python实际导出，advanced/CLI不绑定必须显式登记。

  **Exit / oracle**: 所有本Spec任务完整验收且链接可追溯；当前设计不混入planned；双PDF身份/排版通过；184不被自动勾选。

## Fragmentation Review

18任务按12个可观察批次组织；B0安装/ABI、B2E扩展边界有独立出口；B0C及B1–B5成对组合，B6–B9分别迁移、完整C++资格、Python包装与文档。任务卡按registry实际执行顺序排列，保留原ID；详细分组依据见[执行表](batch-execution.md#allocation-and-closure-decision)。
Runtime与cache不同owner、request与conversation不同持久语义、Provider与Python不同安全/验收边界，因此不合并。
T013是真实集成验收，T014是源码/API/资格交付，不替代前面的行为测试。
