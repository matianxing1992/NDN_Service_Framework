# Work Unit Contracts

**Revision**: 7 | **Status**: planned, all implementation NOT_STARTED

## Common Boundary

O-001已按当前源码与181承接范围关闭；T001仍需关闭O-002--005并冻结可执行接口/依赖/测试选择器后开始实现。
每任务的具体文件与符号由其CD定义，类/方法与字段解释分别引用
[symbol design](symbol-design.md)和[value contracts](value-contracts.md)，不重复抄表。
新增/变更公开API提供英文Doxygen/docstring；重要字段、寿命与安全边界解释含义，
成功/失败/取消等适用用法随实现维护。普通局部helper不改变契约时无需额外设计审批。

执行顺序和短记录统一见 [validation workflow](pre-test-static-review.md)。
T002--T014只做实现、静态审查、相关unit及必要构建，任务[x]仅表示这些完成。
各PO的真实跨组件/跨进程和实验义务一律由T016关闭；本文件LocalChecks不替代完整PO。
集成用例与harness随所属任务编写注册；不能只登记将来写测试的TODO。
具体命令与测试路径见 [test inventory](proof-design.md#planned-test-and-build-inventory)；
unit/integration按实际调用边界分类，不按文件名或smoke标签分类。
T001冻结新unit selectors，不能执行整份混合测试文件而意外启动integration。
必要原生工具链、时间上限、新run目录和原始失败保存沿用proof/plan约定。

实际结果优先记tasks.md；详情才使用evidence/tNNN-completion.md，不另建审查或逐层放行报告。
失败只清理本次资源，保留run/patch并同步tasks及failure index，不覆盖他人工作或历史证据。

## T001 Successor Baseline and Design Closure

- **Outcome**: 冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。
- **Design**: FR-015,FR-016,FR-017; CD-001--014; INV-001,INV-004,INV-008,INV-009; FLOW-001, FLOW-002。
- **Changes**: spec182 文档、native-dependencies.json、compatibility-manifest.json、冻结 fixture 元数据
- **ForbiddenChanges**: 修改 Spec181 状态；无最终 baseline 就启动迁移；把 Python helper 保留为默认。
- **LocalChecks**: 复用revision7源码核对，冻结unit/integration selectors；本轮审计不运行依赖探针。后续设计工作如需可行性探针，单独记实际结果，不计产品验收；动态证据未取得时保持相应OPEN。
- **FinalProof**: PO-012。

## T002 Installable Native Library Contract

- **Outcome**: 先将 existing Provider runtime 构建为可安装库，独立 consumer 链接已有可执行符号；planned requester 仅声明，完整调用由T010实现、T016验收。
- **Design**: FR-001,FR-012; CD-001,CD-009; INV-001,INV-002,INV-007; FLOW-001, FLOW-002。
- **Changes**: NativeInferenceClient公开头、CD-009构建/pc及安装链接consumer；request完整实现由T010负责。
- **ForbiddenChanges**: 复制一份 DI 实现进 extension；以 stub 返回成功冒充完整请求。
- **LocalChecks**: 必要编译/安装/链接检查及已有runtime接口单测；完整request/isolation在T016。
- **FinalProof**: PO-001 的 installed-library L0 子门；完整请求和 isolation 由 T010/T014/T016 收口。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T003 Native Split and Placement Decisions

- **Outcome**: 两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。
- **Design**: FR-003,FR-009,FR-016; CD-002; INV-001,INV-002,INV-003,INV-004; FLOW-001, FLOW-002。
- **Changes**: NativePlanning、NativeQwenPlanner、NativeYoloPlanner 声明/源和 di-native-planning
- **ForbiddenChanges**: Python callback；has_model 代替 exact residency；在 Core 写模型分支。
- **LocalChecks**: 固定小图cover、budget/device/lease/ref排序及非法向量单测；单元级既定检错。
- **FinalProof**: PO-002。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T004 Canonical Native Plan Sealing

- **Outcome**: 合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。
- **Design**: FR-002,FR-004; CD-003; INV-001,INV-003,INV-004; FLOW-001, FLOW-002。
- **Changes**: NativePlanSealer 与 NativeExecutionPlanJson 头/源，di-native-plan-sealer
- **ForbiddenChanges**: 修改 wire/schema；重生成 oracle 掩盖差异；移除 Provider 独立校验。
- **LocalChecks**: canonical JSON/签名字节、非法投影/endpoint/ACK摘要单测；真实Core commit与Provider parser协作在T016。
- **FinalProof**: PO-003。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T005 Native Requester Grant Path

- **Outcome**: 原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。
- **Design**: FR-005; CD-004; INV-001,INV-003,INV-004,INV-005; FLOW-001, FLOW-002。
- **Changes**: NativeGrantClient、NativeArtifactPolicyAuthority 头/源和 di-native-requester-grant
- **ForbiddenChanges**: 新网络权威服务；自写密码算法；保护路径 fallback plaintext。
- **LocalChecks**: grant构造、签名/原因码、wrong-key/recipient/expiry单测；真实authority publication/fetch及Provider消费在T016。
- **FinalProof**: PO-004。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T006 Native Cold ONNX Assembly

- **Outcome**: Selection 后原生装配与既有固定 bytes 一致，消除生产 helper IPC。
- **Design**: FR-006,FR-016; CD-005; INV-003,INV-004,INV-006,INV-007; FLOW-001, FLOW-002。
- **Changes**: NativeCanonicalOnnxAssembler 与 NativeOnnxRecipeAssembler 头/源、di-native-onnx-recipe
- **ForbiddenChanges**: 全部提前离线切分；临时明文绕过授权；改 recipe digest。
- **LocalChecks**: 固定recipe字节、inline/external-data和错误输入单测，可直接调用原生ONNX库；Selection后真实Provider装配在T016。
- **FinalProof**: PO-005。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T007 Native Tokenizer Execution

- **Outcome**: 原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。
- **Design**: FR-007; CD-006; INV-002,INV-006,INV-007; FLOW-001, FLOW-002。
- **Changes**: NativeTokenizer、NativeStandaloneTokenizer 头/源和 decoder factory，di-native-tokenizer
- **ForbiddenChanges**: 仍调用 Python module；只返回 token IDs；每 token fork helper。
- **LocalChecks**: Unicode/special/byte-fallback/digest encode/decode单测；完整adapter请求和进程依赖隔离在T016。
- **FinalProof**: PO-006。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T008 Native Request Preparation and Admission

- **Outcome**: 原生 input→graph→artifact binding 和可信 offer→view 可独立调用；拒绝伪 provenance，无 Python 辅助。
- **Design**: FR-001,FR-002,FR-004,FR-009,FR-016; CD-013; INV-001,INV-002,INV-003,INV-004; FLOW-001。
- **Changes**: 4 个新增生产文件及已列 adapter 接线，约 400--900 行
- **ForbiddenChanges**: caller trusted=true、策略自行 I/O、离线预生成结果替代 runtime、提前角色装配。
- **LocalChecks**: 输入/工件名称绑定、ACK policy与provenance单测；真实publication及准备/准入协作在T016。
- **FinalProof**: PO-013。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T009 Shared Native Provider Host

- **Outcome**: executable 与独立 consumer 共用宿主及同一 NativeProviderRuntime。
- **Design**: FR-001,FR-009,FR-010,FR-012; CD-014; INV-001,INV-002,INV-003,INV-005; FLOW-001,FLOW-004。
- **Changes**: NativeInferenceProvider头/源与DI_NativeProviderExecutable接线，删除重复宿主逻辑；完整提取ACK/lease/readiness/权限与runtime.handler，范围及注册寿命缺口见[CD-014 source boundary](runtime-boundaries.md#current-registration-boundary)。O-004关闭前不得实施。
- **ForbiddenChanges**: 重写 Provider runtime、把管理权限合并进 serving facade、销毁共享 Face、Python runner trampoline。
- **LocalChecks**: host配置、重复注册、stop/共享资源所有权单测；真实NFD注册/ACK/Selection及多入口协作在T016。
- **FinalProof**: PO-014。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T010 Complete Native Request Lifecycle

- **Outcome**: 独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。
- **Design**: FR-001,FR-002,FR-008; CD-001,CD-013,CD-014; INV-001,INV-002,INV-003,INV-005; FLOW-001, FLOW-002。
- **Changes**: NativeInferenceClient 头/源和 DI_NativeRequester.cpp，di-native-request
- **ForbiddenChanges**: 在 Face 线程阻塞规划或 result；以低层 preplanned 调用代替完整 model request。
- **LocalChecks**: client状态机、cancel/deadline/late callback单测；完整真实Core/Provider请求在T016。
- **FinalProof**: PO-001,PO-003,PO-007。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T011 Native Conversation Continuation

- **Outcome**: 原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。
- **Design**: FR-008,FR-016; CD-007; INV-003,INV-004,INV-005; FLOW-001, FLOW-003。
- **Changes**: NativeConversationCoordinator 头/源及 NativeInferenceClient 接线，di-native-conversation
- **ForbiddenChanges**: 新生成运行时；Python journal authority；悄悄丢弃旧会话格式。
- **LocalChecks**: journal/lineage、prefix、cancel、wrong-parent、replacement状态转换单测；真实两轮续接在T016。
- **FinalProof**: PO-007,PO-008。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T012 Thin Python Native Bindings

- **Outcome**: 支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。
- **Design**: FR-010; CD-008,CD-009; INV-002,INV-004,INV-005,INV-007; FLOW-001, FLOW-004。
- **Changes**: di_bindings.cpp、_ndnsf.cpp、setup.py 和五个已列 Python 导出/入口，test_spec182_native_bindings
- **ForbiddenChanges**: 重新编译复制 DI 源；Python override；重新计算计划/判定成功。
- **LocalChecks**: 绑定参数/异常/生命周期映射及禁止callback单测、必要同库链接检查；真实C++/Python请求一致性在T016。
- **FinalProof**: PO-009。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T013 Default Route and Legacy Retirement

- **Outcome**: 所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。
- **Design**: FR-011,FR-016; CD-010; INV-002,INV-004,INV-005,INV-007; FLOW-001, FLOW-002。
- **Changes**: CD-010 全部列明 caller/runners 和旧路径，test_spec182_legacy_exclusion
- **ForbiddenChanges**: 批量删除未知 consumers；移动算法到工具包后继续默认调用；双默认。
- **LocalChecks**: 调用清单、默认路由和legacy排除逻辑单测；阻断旧模块后的真实maintained callers运行在T016。
- **FinalProof**: PO-010。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T014 Runtime Dependency Exclusion Gate

- **Outcome**: harness 将被测 native scope 与 Python harness 隔离；交付正式 MiniNDN harness/collector/fixtures，本地单测完成后进入T015，真实反例由T016运行。
- **Design**: FR-001,FR-011,FR-012,FR-014; CD-011; INV-002,INV-007,INV-008; FLOW-001, FLOW-002。
- **Changes**: run-spec182-native-closure.py、test_spec182_native_closure.py、NDNSF_DI_NativeClosure_Minindn.py、case-manifest
- 具体函数/内部状态/manifest字段与I01--I08 selector按[native isolation design](native-isolation-design.md)执行；复用同一runner/collector，不建立第二份判定逻辑。
- **ForbiddenChanges**: 只看 PATH/字符串就 PASS；隐藏 Python 服务；混用源/依赖身份。
- **LocalChecks**: collector判定、manifest和隔离gate解析逻辑单测，编写并注册所有真实反例；warm-only/rename-helper/embedded-libpython与隔离运行均在T016。
- **FinalProof**: PO-001,PO-010,PO-012。 本任务只完成局部单测；其余运行证据由T016统一产生。

## T015 Design-code Convergence Audit

- **Outcome**: 逐 FR/CD/INV/PO 核对生产接线、effective config、依赖/源码身份，控制性发现清零。
- **Design**: FR-013; CD-001--014; INV-001,INV-002,INV-003,INV-004,INV-005,INV-006,INV-007,INV-008; FLOW-001, FLOW-002。
- **Changes**: 在同一任务结果中记录整体审查发现，必要时同步受影响契约与追踪；不新增重复audit报告。
- **ForbiddenChanges**: 把存在 helper/单测通过当作完整资格；为通过放宽验收。
- **LocalChecks**: 整体静态检查生产接线、设计、测试/oracle/harness及未关闭义务，不执行集成/实验。
- **FinalProof**: PO-001--016。

## T016 Local Native Qualification

- **Outcome**: 同源完整 suites、YOLO/Qwen MiniNDN 和 no-Python 全部通过。
- **Design**: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011; INV-001,INV-002,INV-003,INV-004,INV-005,INV-006,INV-007,INV-008; FLOW-001, FLOW-002。
- **Changes**: 记录真实suite/case结果索引，复用已完成的harness；修复回到受影响代码/测试契约，不靠放宽判据通过。
- **ForbiddenChanges**: 运行中延长 deadline/改 oracle；将 collector 故障记拒绝；SIF/Tiger 混作本地门。
- **LocalChecks**: 完整unit→真实integration→MiniNDN/no-Python；PO-001--014及Negative Path Matrix全部既定负例/反事实按真实边界运行。
- **FinalProof**: PO-001--016。

## T017 Native Development Handoff

- **Outcome**: 唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。
- **Design**: FR-014,FR-015; CD-012; INV-001,INV-002,INV-007,INV-008,INV-009; FLOW-001, FLOW-002。
- **Changes**: 三个CD-012 docs和evidence/development-handoff.md；复用Experiments/TigerCluster现有交付工具及root skills，只生成182的新身份/依赖/运行树清单，不另建并行SIF工具或重复closure报告。若现有模板不支持新库/原生依赖，先在CD-009/CD-012登记精确模板/脚本修改，外部owner构建与实验。
- **ForbiddenChanges**: 把 SIF/Tiger/GPU 未运行写 PASS；改冻结历史/181 关闭状态。
- **LocalChecks**: 核对交付版本、既有T016结果、链接和可复现命令；无行为变化不重新运行套件。
- **FinalProof**: PO-012。
