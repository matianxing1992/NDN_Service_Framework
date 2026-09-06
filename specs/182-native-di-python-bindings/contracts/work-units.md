# Work Unit Contracts

**Revision**: 1 | **Status**: planned, all implementation NOT_STARTED
本表定义设计批次，不授权现在执行。O-001--004 关闭前全部实现 BLOCK。
T001 必须把超过合理范围的批次再细分为有独立行为和验收的原子单元，并同步全部 ID 引用，
才能标 READY_FOR_IMPLEMENTATION；不可把本表的大估算直接当无限实现授权。

## Common Boundary

ExactFiles/ExactSymbols 以每单元 CD 的全路径清单为规范，测试路径见
[proof inventory](proof-design.md#planned-test-and-build-inventory)。
每任务均包含自己的 RED/实现/GREEN/相邻回归/证据，不另造机械测试任务。

ExactCommands：均为 proof-design 中明确 planned 的入口；cwd=repo root，环境为经 T001
封存的 compiler/ORT/native dependencies，run output 必须新目录。
focused supervisor 120s，初始网络 180s，cleanup 15s；T001 按既有 case 契约冻结，
不得运行中延长。C++ selectors 尚待 T001 注册冻结，这也是当前实施 BLOCK 的原因。

Default EscalationConditions：未列文件/符号/参数；改变 API/wire/状态 owner/依赖/错误或 oracle；
diff 超估算；测试只在编译/环境层失败。先保留证据并修订相关 CD/T/PO，再实施。
普通内部设计修订不等于反复请用户审批；越出用户授权才询问。

RecoveryPoint：失败保留独立 run 和 patch，不 reset/clean/stash 他人工作，不覆盖旧日志；
清理本次自有进程/资源，先更新 evidence/tasks/failure index 再重试。安全秘密不入 Git。
只有完成本任务所有必要 proof 并记录真实 CommandsExecuted，才勾选任务。

ExpectedDiff 是偏移提示，不是配额。T003/004/006/008/009 可能大于 300 行，
原因是既有算法/状态和字节契约较大；T001 须确认可独立 checkpoint 的分界后细化。
T010/T011 涉及多个薄调用方，文件数大是入口迁移而非允许新功能。
每单元实际结果采用 proof-design 的 Completion Evidence Record。

## T001 Successor Baseline and Design Closure

- **Owner**: 本机，Successor Baseline and Design Closure。
- **Design**: FR-015,FR-016; CD-001--012; INV-001,INV-004,INV-008,INV-009; FLOW-001, FLOW-002。
- **StartState**: Spec181 local closure；核对具体 source/design identity。
- **EndState**: 冻结 181 交付、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--004；修订叶子签名与任务至可执行。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-001--012 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T001。
- **DecisionBudget**: ZERO。
- **AllowedDecisions**: 仅原样引用已验证字段/签名与事实；未知依赖通过最多两个候选的有界设计调查；不运行模型实验。
- **ForbiddenChanges**: 修改 Spec181 状态；无最终 baseline 就启动迁移；把 Python helper 保留为默认。
- **ExpectedDiff**: spec182 文档、native-dependencies.json、compatibility-manifest.json、冻结 fixture 元数据；0 生产文件；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-012。
- **ExactCommands / VerificationLadder**: L0 source/design review；设计 probe 若另行执行必须独立记录，不能计实现；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 设计 READY 需要关闭每个影响接口/状态/依赖的 OPEN；同样适用 common boundary。
- **RecoveryPoint**: 当前设计 commit；保留 181 authority。
- **Evidence**: ../evidence/t001-completion.md（planned，当前不存在）。

## T002 Installable Native Library Contract

- **Owner**: 本机，Installable Native Library Contract。
- **Design**: FR-001,FR-012; CD-001,CD-009; INV-001,INV-002,INV-007; FLOW-001, FLOW-002。
- **StartState**: T001；核对具体 source/design identity。
- **EndState**: 原生库可安装，独立 consumer 可链接已冻结类型和生命周期接口，无 Python 链接依赖。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-001,CD-009 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T002。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 局部 include/构建变量名和等价编译修正；保持目标名和链接 owner。
- **ForbiddenChanges**: 复制一份 DI 实现进 extension；以 stub 返回成功冒充完整请求。
- **ExpectedDiff**: NativeInferenceClient 头/源及 CD-009 构建/pc 文件，测试 di-native-client；约 100--300 行生产变更；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-001。
- **ExactCommands / VerificationLadder**: L0/L1/L2/L6；native build target + consumer dependency checks；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 隐藏链接依赖、增加 ABI/公共字段需回 CD-001/009；同样适用 common boundary。
- **RecoveryPoint**: 独立库可链接 checkpoint；未接通 request 明确 NOT_AVAILABLE。
- **Evidence**: ../evidence/t002-completion.md（planned，当前不存在）。

## T003 Native Split and Placement Decisions

- **Owner**: 本机，Native Split and Placement Decisions。
- **Design**: FR-003,FR-009,FR-016; CD-002; INV-001,INV-002,INV-003,INV-004; FLOW-001, FLOW-002。
- **StartState**: T002；核对具体 source/design identity。
- **EndState**: 两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-002 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T003。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 移植现有已冻结算法及确定性排序；局部容器选择保持语义。
- **ForbiddenChanges**: Python callback；has_model 代替 exact residency；在 Core 写模型分支。
- **ExpectedDiff**: NativePlanning、NativeQwenPlanner、NativeYoloPlanner 声明/源和 di-native-planning；6 生产文件，约 400--900 行；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-002。
- **ExactCommands / VerificationLadder**: L1/L2/L6，图 cover/预算/device/lease/ref ordering vectors；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 超过范围或发现算法缺口先拆成稳定策略接口单元，修订 CD-002；同样适用 common boundary。
- **RecoveryPoint**: 每策略向量闭合的隔离 checkpoint，默认请求尚未切换。
- **Evidence**: ../evidence/t003-completion.md（planned，当前不存在）。

## T004 Canonical Native Plan Sealing

- **Owner**: 本机，Canonical Native Plan Sealing。
- **Design**: FR-002,FR-004; CD-003; INV-001,INV-003,INV-004; FLOW-001, FLOW-002。
- **StartState**: T003；核对具体 source/design identity。
- **EndState**: 合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-003 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T004。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 等价编码实现；保持固定字节和现有验证顺序。
- **ForbiddenChanges**: 修改 wire/schema；重生成 oracle 掩盖差异；移除 Provider 独立校验。
- **ExpectedDiff**: NativePlanSealer 与 NativeExecutionPlanJson 头/源，di-native-plan-sealer；4 生产文件，约 300--700 行；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-003。
- **ExactCommands / VerificationLadder**: L1/L2/L3/L6，frozen wire + Core commit + real parser；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 字节差异或新字段需先回 CD-003/O-004；同样适用 common boundary。
- **RecoveryPoint**: 固定 vectors + Core commit 边界通过后 checkpoint。
- **Evidence**: ../evidence/t004-completion.md（planned，当前不存在）。

## T005 Native Requester Grant Path

- **Owner**: 本机，Native Requester Grant Path。
- **Design**: FR-005; CD-004; INV-001,INV-003,INV-004,INV-005; FLOW-001, FLOW-002。
- **StartState**: T004；核对具体 source/design identity。
- **EndState**: 原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-004 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T005。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 调用现有密码原语和 Core publication；等价错误映射。
- **ForbiddenChanges**: 新网络权威服务；自写密码算法；保护路径 fallback plaintext。
- **ExpectedDiff**: NativeGrantClient、NativeArtifactPolicyAuthority 头/源和 di-native-requester-grant；4 生产文件，约 250--550 行；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-004。
- **ExactCommands / VerificationLadder**: L1/L2/L3/L6，真实签名 publication/fetch 与 wrong-key/recipient/expiry；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 缺少现有原语或需改变 key owner，先修订 CD-004；同样适用 common boundary。
- **RecoveryPoint**: 密钥清理、正负向完整后 checkpoint。
- **Evidence**: ../evidence/t005-completion.md（planned，当前不存在）。

## T006 Native Cold ONNX Assembly

- **Owner**: 本机，Native Cold ONNX Assembly。
- **Design**: FR-006,FR-016; CD-005; INV-003,INV-004,INV-006,INV-007; FLOW-001, FLOW-002。
- **StartState**: T002；O-002 closed；核对具体 source/design identity。
- **EndState**: Selection 后原生装配与既有固定 bytes 一致，消除生产 helper IPC。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-005 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T006。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 按锁定 ONNX/protobuf API 实现既定 recipe；等价路径/size 检查。
- **ForbiddenChanges**: 全部提前离线切分；临时明文绕过授权；改 recipe digest。
- **ExpectedDiff**: NativeCanonicalOnnxAssembler 与 NativeOnnxRecipeAssembler 头/源、di-native-onnx-recipe；4 生产文件，约 300--800 行并删除 helper 分支；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-005。
- **ExactCommands / VerificationLadder**: L1/L2/L3/L6，双 recipe cold、inline/external-data、实际 ONNX load；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 字节或资源行为不等价，回 O-002/CD-005，不能重封 oracle；同样适用 common boundary。
- **RecoveryPoint**: 原 helper 仅 reference；保留失败 staging 诊断，秘密清理。
- **Evidence**: ../evidence/t006-completion.md（planned，当前不存在）。

## T007 Native Tokenizer Execution

- **Owner**: 本机，Native Tokenizer Execution。
- **Design**: FR-007; CD-006; INV-002,INV-006,INV-007; FLOW-001, FLOW-002。
- **StartState**: T002；O-003 closed；核对具体 source/design identity。
- **EndState**: 原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-006 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T007。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 仅锁定 native API 的封装与资源释放；不自改 tokenizer 算法。
- **ForbiddenChanges**: 仍调用 Python module；只返回 token IDs；每 token fork helper。
- **ExpectedDiff**: NativeTokenizer、NativeStandaloneTokenizer 头/源和 decoder factory，di-native-tokenizer；5 生产文件，约 150--350 行；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-006。
- **ExactCommands / VerificationLadder**: L1/L2/L3/L6，Unicode/special/byte fallback/digest vectors；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 需新 C ABI 或改变线程安全/ownership，先修订 O-003/CD-006；同样适用 common boundary。
- **RecoveryPoint**: native decoder 向量和资源检查后 checkpoint。
- **Evidence**: ../evidence/t007-completion.md（planned，当前不存在）。

## T008 Complete Native Request Lifecycle

- **Owner**: 本机，Complete Native Request Lifecycle。
- **Design**: FR-001,FR-002,FR-008; CD-001; INV-001,INV-002,INV-003,INV-005; FLOW-001, FLOW-002。
- **StartState**: T003/T004/T005/T006/T007；核对具体 source/design identity。
- **EndState**: 独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-001 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T008。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 在已定义串行 executor 内接线既有原生组件；不新造全局状态。
- **ForbiddenChanges**: 在 Face 线程阻塞规划或 result；以低层 preplanned 调用代替完整 model request。
- **ExpectedDiff**: NativeInferenceClient 头/源和 DI_NativeRequester.cpp，di-native-request；3 生产文件，约 300--700 行；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-001,PO-003,PO-007。
- **ExactCommands / VerificationLadder**: L1/L2/L3/L6，真实 Core/Provider；未到 T013 不作正式矩阵声明；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 需要新 callback/状态/取消 API，先修订 CD-001/007；同样适用 common boundary。
- **RecoveryPoint**: 完整最小原生请求及生命周期 focused PASS checkpoint。
- **Evidence**: ../evidence/t008-completion.md（planned，当前不存在）。

## T009 Native Conversation Continuation

- **Owner**: 本机，Native Conversation Continuation。
- **Design**: FR-008,FR-016; CD-007; INV-003,INV-004,INV-005; FLOW-001, FLOW-003。
- **StartState**: T008；核对具体 source/design identity。
- **EndState**: 原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-007 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T009。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 按冻结旧状态和持久化契约接线；保持一个 writer。
- **ForbiddenChanges**: 新生成运行时；Python journal authority；悄悄丢弃旧会话格式。
- **ExpectedDiff**: NativeConversationCoordinator 头/源及 NativeInferenceClient 接线，di-native-conversation；3 生产文件，约 300--800 行；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-007,PO-008。
- **ExactCommands / VerificationLadder**: L1/L2/L3/L6，真实两轮/取消/错 parent/有限 replacement；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 新增状态或恢复语义，回 CD-007/O-004；同样适用 common boundary。
- **RecoveryPoint**: 有效 checkpoint 不能被失败 turn 覆盖；保留失败证据。
- **Evidence**: ../evidence/t009-completion.md（planned，当前不存在）。

## T010 Thin Python Native Bindings

- **Owner**: 本机，Thin Python Native Bindings。
- **Design**: FR-010; CD-008,CD-009; INV-002,INV-004,INV-005,INV-007; FLOW-001, FLOW-004。
- **StartState**: T009；核对具体 source/design identity。
- **EndState**: 支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-008,CD-009 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T010。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 纯类型转换、GIL/observer 投递及冻结错误映射。
- **ForbiddenChanges**: 重新编译复制 DI 源；Python override；重新计算计划/判定成功。
- **ExpectedDiff**: di_bindings.cpp、_ndnsf.cpp、setup.py 和五个已列 Python 导出/入口，test_spec182_native_bindings；约 150--400 行 native 与删减 Python；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-009。
- **ExactCommands / VerificationLadder**: L0/L1/L2/L3/L6，固定向量和真实入口一致性；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 未列公开签名/consumer 或需新 fallback，回 CD-008/O-004；同样适用 common boundary。
- **RecoveryPoint**: 同库两个入口 focused PASS，旧入口清单尚未全部退出。
- **Evidence**: ../evidence/t010-completion.md（planned，当前不存在）。

## T011 Default Route and Legacy Retirement

- **Owner**: 本机，Default Route and Legacy Retirement。
- **Design**: FR-011,FR-016; CD-010; INV-002,INV-004,INV-005,INV-007; FLOW-001, FLOW-002。
- **StartState**: T010；核对具体 source/design identity。
- **EndState**: 所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-010 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T011。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 按冻结兼容映射转换调用；删除已证实无生产消费者的路径。
- **ForbiddenChanges**: 批量删除未知 consumers；移动算法到工具包后继续默认调用；双默认。
- **ExpectedDiff**: CD-010 全部列明 caller/runners 和旧路径，test_spec182_legacy_exclusion；生产删除量由 O-004 inventory 固定；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-010。
- **ExactCommands / VerificationLadder**: L0/L2/L3/L6，callback-aware inventory + old module denial；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 发现新 consumer 先补 manifest/PO，不按文件名推断 dead code；同样适用 common boundary。
- **RecoveryPoint**: 逐入口切换记录和完整去向清单，可回到此前文档 checkpoint重修。
- **Evidence**: ../evidence/t011-completion.md（planned，当前不存在）。

## T012 Runtime Dependency Exclusion Gate

- **Owner**: 本机，Runtime Dependency Exclusion Gate。
- **Design**: FR-001,FR-011,FR-012,FR-014; CD-011; INV-002,INV-007,INV-008; FLOW-001, FLOW-002。
- **StartState**: T011；核对具体 source/design identity。
- **EndState**: harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-011 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T012。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 在 O-005 已冻结隔离方法内实现有界观察；明确 PID/namespace owner。
- **ForbiddenChanges**: 只看 PATH/字符串就 PASS；隐藏 Python 服务；混用源/依赖身份。
- **ExpectedDiff**: run-spec182-native-closure.py、test_spec182_native_closure.py、case-manifest；0 生产运行时代码，约 150--350 行 harness；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-001,PO-010,PO-012。
- **ExactCommands / VerificationLadder**: L0/L3/L6；warm-only、rename-helper、embedded-libpython 反例；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 隔离工具无法观察旁路，保留 BLOCK 并回 O-005；同样适用 common boundary。
- **RecoveryPoint**: 退出/cleanup 完整；反例 run 分开保留。
- **Evidence**: ../evidence/t012-completion.md（planned，当前不存在）。

## T013 Design-code Convergence Audit

- **Owner**: 本机，Design-code Convergence Audit。
- **Design**: FR-013; CD-001--012; INV-001,INV-002,INV-003,INV-004,INV-005,INV-006,INV-007,INV-008; FLOW-001, FLOW-002。
- **StartState**: T012；核对具体 source/design identity。
- **EndState**: 逐 FR/CD/INV/PO 核对生产接线、effective config、依赖/源码身份，控制性发现清零。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-001--012 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T013。
- **DecisionBudget**: ZERO。
- **AllowedDecisions**: 仅审查和记录证据；修复必须回所属任务。
- **ForbiddenChanges**: 把存在 helper/单测通过当作完整资格；为通过放宽验收。
- **ExpectedDiff**: spec182 audit/traceability/evidence/post-implementation-audit.md；0 生产文件；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-001--012。
- **ExactCommands / VerificationLadder**: L0/L2 source-aware audit，未运行的 L4 明确 NOT_RUN；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 任何 semantic/security/wiring/config/evidence gap 为 BLOCK；同样适用 common boundary。
- **RecoveryPoint**: 记录 first open boundary，返回所属任务修复后复审。
- **Evidence**: ../evidence/t013-completion.md（planned，当前不存在）。

## T014 Local Native Qualification

- **Owner**: 本机，Local Native Qualification。
- **Design**: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011; INV-001,INV-002,INV-003,INV-004,INV-005,INV-006,INV-007,INV-008; FLOW-001, FLOW-002。
- **StartState**: T013 PASS；核对具体 source/design identity。
- **EndState**: 同源完整 suites、YOLO/Qwen MiniNDN 和 no-Python 全部通过。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-011 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T014。
- **DecisionBudget**: ZERO。
- **AllowedDecisions**: 仅执行已冻结 case/selector/config/timeout；失败先保留诊断。
- **ForbiddenChanges**: 运行中延长 deadline/改 oracle；将 collector 故障记拒绝；SIF/Tiger 混作本地门。
- **ExpectedDiff**: NDNSF_DI_NativeClosure_Minindn.py、fixtures、evidence/local-qualification.md；0 生产修复允许；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-001--011。
- **ExactCommands / VerificationLadder**: L4/L5 同源真实 MiniNDN；L1/2/3/L6 已有完整有效证据；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 失败更新 raw/evidence/failure index，回最早受影响任务和 T013；同样适用 common boundary。
- **RecoveryPoint**: 每 case 独立新目录；无未收集进程和明文/密钥残留。
- **Evidence**: ../evidence/t014-completion.md（planned，当前不存在）。

## T015 Native Development Handoff

- **Owner**: 本机，Native Development Handoff。
- **Design**: FR-014,FR-015; CD-012; INV-001,INV-002,INV-007,INV-008,INV-009; FLOW-001, FLOW-002。
- **StartState**: T014 PASS；核对具体 source/design identity。
- **EndState**: 唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。
- **ExactFiles / ExactSymbols**: [code-design](code-design.md) 的 CD-012 和 [proof inventory](proof-design.md#planned-test-and-build-inventory) 的 T015。
- **DecisionBudget**: LOW。
- **AllowedDecisions**: 仅按已验证实现更新使用说明/身份字段，不扩大 claims。
- **ForbiddenChanges**: 把 SIF/Tiger/GPU 未运行写 PASS；改冻结历史/181 关闭状态。
- **ExpectedDiff**: 三个 CD-012 docs 和 evidence/development-handoff.md、closure-record.md；0 生产文件；测试约 80--300 行/行为，文档按实际证据；不按比例凑改动。
- **ProofObligations**: PO-012。
- **ExactCommands / VerificationLadder**: L0/L5 追踪、链接、commit/工件清单核对；使用上述 planned command contract，未冻结 selectors 前 BLOCK。
- **EscalationConditions**: 接收方缺源码/依赖或 audit identity 漂移，回对应 gate；同样适用 common boundary。
- **RecoveryPoint**: 本地 checkpoint；不 push/远端提交，记录下一外部 owner。
- **Evidence**: ../evidence/t015-completion.md（planned，当前不存在）。
