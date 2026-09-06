# Feature Specification: Native NDNSF-DI with Optional Python Bindings

**Feature Branch**: `Experimental`
**Feature Directory**: `182-native-di-python-bindings`
**Created**: 2026-09-06
**Revision**: 5
**Status**: DRAFT
**Execution Status**: NOT_STARTED
**Activation**: active design; merged baseline stabilization in progress

**Input**: 所有者要求 C++ 自身完成完整 NDNSF-DI 调用；Python 只作为可选兼容外壳。
Python 可以传入原生策略对象或配置，但默认策略执行、切分决策、运行时装配和协作调用
必须在原生实现中完成。当前已经进入合并修复阶段；本轮仅更新设计技能与182文档，不执行原生迁移或实验。

## Goal

提供一个可被纯 C++ 应用链接和调用的 NDNSF-DI 库。相同的模型、输入和策略也可由
Python 绑定提交，两个入口使用同一套 DI 实现，包括模型/输入准备、ACK admission 和 Provider 服务注册。移除默认生产路径对 Python 规划器、
状态机、装配 helper 和 tokenizer helper 的运行时依赖。

本需求改变的是 DI 的语言实现与职责归属，保留 Request/ACK/Selection/Response、
ACK 驱动规划、选定 Provider 独立验证、受保护工件、按需装配及共享运行时语义。
不把模型规划、Qwen KV/tokenizer 或 YOLO 后处理移入通用 NDNSF Core。

### Relationship to Spec181

- 用户已调整顺序：先完成合并修复与必要基线检查，再开展182；不再要求先完成181全部旧资格实验。
- 182 不宣布181完成。T001 记录已交付能力、保留回归、待迁移验证及外部实验的承接表，冻结合并后的源码与依赖身份。
- 合并工作区的 unit/integration 仍有失败。文档可现在修订；实现从已确认的合并 checkpoint 和关闭的设计门开始。旧 PASS 不替代182验收。
- 本机负责代码、unit/integration/MiniNDN 与开发交付；实验机器负责 SIF/Tiger，外部执行标 TRANSFERRED。
- 当前 source-of-truth 与未提交合并身份见 [merged baseline](evidence/merged-source-baseline-r3.json)；活动指针已经指向182，旧 managed plan 需同步。

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Standalone Native Invocation (Priority: P1)

C++ 应用开发者只链接原生 DI 库并提供配置、模型引用、输入与策略，即可完成受保护的
分布式推理；运行机器无需 Python 解释器、DI Python 包或后台 Python 服务。

**Why this priority**: 完整原生入口是功能目标，原生 Provider 存在不能替代它。
**Independent Test**: 在不含 Python 的本地运行环境中，纯 C++ requester 和 Providers
完成小模型冷启动、真实 grant、装配、角色间依赖传输及数值结果。

**Acceptance Scenarios**:
1. **Given** 已认证 canonical 小模型工件与空缓存，**When** C++ 应用发起请求，
   **Then** 在 ACK_CLOSED 后规划、通过既有 Core 提交 Selection、冷装配并返回 oracle 一致结果。
2. **Given** 无兼容 Provider 或错误 grant，**When** 执行请求，
   **Then** 在对应规划/授权边界拒绝，不加载未授权模型，不声称网络超时为预期拒绝。
3. **Given** 运行根目录无 Python/libpython/DI Python 包，**When** 执行冷路径，
   **Then** 全链成功且未启动解释器、嵌入解释器或请求隐藏 Python 服务。

### User Story 2 - Native Strategy Selection (Priority: P1)

调用方传入策略对象或配置表达希望采用的方法；C++ 调用策略实现产生切分候选，
C++ placement 策略决定候选中的 Provider/device，C++ 校验与执行其结果。

**Why this priority**: “传一个方法类”不能变成 C++ 回调 Python 业务算法。
**Independent Test**: 同一认证图与冻结 ACK 输入，两个原生策略产生可区分且合法的候选，
策略非法输出被独立校验拒绝；Python 子类/callable 不能作为默认原生策略。

**Acceptance Scenarios**:
1. **Given** 原生 strategy 对象，**When** 从 C++ 或 Python 绑定调用，
   **Then** 相同确定性输入产生相同候选/放置，不执行 Python override。
2. **Given** 不完整 layer/rank cover 或越界 node index，**When** 策略返回候选，
   **Then** 原生 DI 在封印前拒绝，不允许策略绕过授权或直接提交 Selection。

### User Story 3 - Compatible Python Entry (Priority: P2)

Python 用户继续用短 API 调用，但模型规划、请求状态及结果判定均来自原生 DI。
配置解析、类型转换、事件投递和展示可以在 Python，第二套业务状态机不可以。

**Independent Test**: C++ 与 Python 两个入口运行同一固定输入；
计划规范字节、错误类别、输出和终态一致。阻断旧 Python coordinator 后默认请求仍成功。

**Acceptance Scenarios**:
1. **Given** 支持的旧公开调用签名，**When** 经兼容映射调用，
   **Then** 转成原生请求且保留约定的返回/异常语义。
2. **Given** Python callback 策略或未覆盖旧能力，**When** 试图调用，
   **Then** 显式报告迁移不支持，禁止静默退回旧运行时；全量完成前必须解决迁移清单。

### User Story 4 - Stateful Native Generation (Priority: P2)

Qwen 文本生成、取消、会话续接和已支持的有限恢复使用原生 DI 调度与模型 adapter，
Python 不参与每 token/epoch 或会话提交决策。

**Independent Test**: 小型 Qwen-compatible fixture，三阶段 rank-one pipeline，
原生 tokenizer 解码；同一 prompt/seed 对照冻结 token/text oracle，并验证取消和续接。

**Acceptance Scenarios**:
1. **Given** 空缓存和 standalone tokenizer 工件，**When** 原生请求生成文本，
   **Then** 完成首次装配、token 输出及文本解码，不 fork Python。
2. **Given** 运行中取消或过期状态，**When** 到达下一调度/提交边界，
   **Then** 停止新工作，拒绝旧 attempt/state，输出分别记录本地取消终态和远端 deadline/control 收束的清理结果，不承诺本地取消立即中止远端。

### User Story 5 - Reproducible Development Delivery (Priority: P3)

实验机器取得一个提交及依赖/模型/配置清单，即可独立构建和运行同一 C++ DI。
本机交付本地验收证据，容器与集群结果由外部 owner 单独产出。

**Independent Test**: 干净 checkout 的独立 C++ consumer 构建/链接/运行；
Python 绑定可选构建，禁止依赖主工作区未提交文件。

### Edge Cases

- 策略返回零候选、超预算、重复 role/rank、非 ingress 输入或多终端角色。
- ACK 后设备/租约失效：Provider 仍独立拒绝；请求方排序不授予执行权。
- 授权成功但装配失败、密文/外部权重被篡改、错误内容密钥、取消期间晚到回调。
- 非法跨 attempt/grant/state 绑定、重复 commit、事件乱序、销毁 client 时请求未结束。
- 无 Python 时冷缓存装配和完整文本输出；只有 warm cache 或 token IDs 不构成通过。
- tokenizer/protobuf 版本变化导致字节或 token 差异：拒绝混用身份，不能改 oracle 过门。
- Core Data 包大小限制：原生化不豁免完整签名 wire 大小和负载资源界限。

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: **Independent Native Invocation**. System MUST 从纯 C++ API 完成模型引用到结果的
  整条受保护 DI 调用；缺失资源在对应边界失败，不借助 Python 运行时补齐。
- **FR-002**: **Core Mechanism Reuse**. System MUST 复用现有 BeginCollaboration /
  CommitCollaborationPlan 和 Provider CollaborationContext；不得新建平行协议或模型专用 Core API。
- **FR-003**: **Native Strategy Execution**. System MUST 接受原生策略对象/配置，
  由 C++ 执行 split 和 placement；拒绝 Python callback 作为默认生产策略。
- **FR-004**: **Single Plan Semantics**. System MUST 由 C++ DI 构造并校验候选、依赖、
  规范摘要、grant view 和 Selection 投影，并在规划前原生验证 ACK provenance 与 offer policy；非法图/绑定在网络提交前拒绝。
- **FR-005**: **Protected Grant Closure**. System MUST 原生完成 requester grant 请求/绑定/
  发布和 Provider 验证/密钥消费/清理，保留 Spec170/181 保护语义及独立权威职责。
- **FR-006**: **Native Cold Assembly**. System MUST 在选定 Provider 原生完成所需动态 ONNX
  装配与外部权重处理；必须保持认证 recipe、资源界限、加密暂存和装配字节契约。
- **FR-007**: **Native Text Processing**. System MUST 在原生 adapter 完成所需 tokenizer
  encode/decode；拒绝错误 tokenizer 身份，不启动 Python helper。
- **FR-008**: **Shared Native Lifecycle**. System MUST 由原生 owner 完成请求/attempt/
  取消/生成/续接与结果提交；Python 只投递和呈现事件，不推进平行业务状态。
- **FR-009**: **Model Boundary Preservation**. System MUST 保持 YOLO/Qwen 共用机制，
  模型图、切分细节、tokenizer 与后处理留在 native adapters，不反向耦合通用 Core。
- **FR-010**: **Thin Python Compatibility**. System MUST 将支持的 Python API 转发至同一
  原生库及 Provider host；Python callable strategy/runner 明确迁移为原生对象；类型/参数错误在入口报告，运行错误映射原生类别，不静默 Python fallback。
- **FR-011**: **Default Path Retirement**. System MUST 完成旧 Python 路径及其真实调用方的
  迁移清单；退出默认 import/运行图并有阻断旧实现仍可运行的证明。
- **FR-012**: **Native Build and Runtime Closure**. System MUST 可独立构建/安装/链接原生库
  与 C++ consumer；生产运行树不依赖 Python/libpython/DI Python 包，绑定为可选构建产物。
- **FR-013**: **Behavioral Proof and Convergence**. System MUST 在相应范围静态读码审查PASS后依次执行真实unit/integration/MiniNDN与规定负例；完整本地验收另需T015整体audit PASS，故障先分类首边界。
- **FR-014**: **Immutable Local Delivery**. System MUST 交付同一源、原生库、配置、工件、
  adapter、harness、依赖锁与证据身份；本地交付和外部 SIF/Tiger verdict 分开记录。
- **FR-015**: **Controlled Successor Activation**. System MUST 在合并修复基线及181承接表确认、对应设计门关闭后启动182实现；不要求先完成181全部旧资格，不改写历史验收结果。
- **FR-016**: **No Capability Reduction by Relocation**. System MUST 保留已支持生产能力的
  行为清单；禁止以提前离线固定切分、只验 warm path、只返回 tokens 或删除负例冒充完整原生化。

- **FR-017**: **Symbol Documentation and Usage Closure**. System MUST 在每个实施单元开始前冻结受影响类、方法、字段和关键局部状态的职责、变更原因、签名、类型/单位/边界、所有权、失败/取消、调用方、注释及前后用法；新增/修改/复用/退出路径均可追踪，未决项阻塞对应实现。不得用堆砌符号名或转述方法名代替解释。

- **FR-018**: **Pre-Test Static Code Review**. System MUST 在每个实施单元及unit/integration/MiniNDN入口执行或核对有效的S0静态代码审查：实际阅读生产/测试逻辑并对照设计，记录源码证据、问题与修复复审、subject身份和允许测试范围。未审、BLOCK、STALE或范围不足不得运行；代码/设计/测试等行为变化须使相关审查失效。lint/编译/文档扫描不能替代，具名RED只按受控审查契约放行。

- **FR-019**: **Adversarial Verification Closure**. System MUST 在S0完成Design Conformance/Correctness/Test Adequacy三层审查及具源码依据的5风险预测（不足须解释），逐风险映射runtime观察量、检错测试/断言和PO；测试通过后S1再次检查假绿、实际风险证据与最终diff。必要行为未验证不算完成，无已知阻塞则立即进入计划验证，禁止无证据无限审查/重构。

### Key Entities

- NativeInferenceClient / NativeInferenceHandle：原生调用入口与请求操作句柄。
- NativeModelSplitStrategy / NativePlacementStrategy：只生成提案的原生策略，不能发布网络消息。
- NativeModelAdapter：认证图、任务/状态语义及模型差异的原生接口。
- NativePlanningSnapshot / NativePlanSealer：固定输入与计划语义的唯一构造/校验方。
- NativeGrantClient / NativeArtifactPolicyAuthority：requester grant 协调与独立策略权威。
- NativeCanonicalOnnxAssembler / NativeTokenizer：运行期格式操作；不持有另一套协作状态。
- CompatibilityManifest / DevelopmentHandoff：旧能力迁移去向与交付身份记录。

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: **Zero Runtime Python Dependency**. 纯 C++ requester/Providers 在无解释器运行树中
  完成冷装配与完整文本输出；解释器执行/嵌入、Python 服务依赖均为零。
- **SC-002**: **Equivalent Public Entrypoints**. 固定请求/时钟/随机材料/ACK snapshot 的对照中，
  C++ 与 Python 绑定的规范计划字节、拒绝类别及终态一致；实时运行按独立 oracle 比较结果。
- **SC-003**: **Independent Strategy Validation**. 每个默认策略至少一个成功向量和一个非法
  候选反例，移除关键校验的错误实现必须在语义断言处失败。
- **SC-004**: **Protected Cold-path Parity**. 正确 grant/装配成功；过期、错误接收者、
  伪签名、错误密钥、篡改工件在已注册边界拒绝；清理、装配字节与既有 oracle 一致。
- **SC-005**: **Stateless and Stateful Closure**. YOLO 原子/分布式/七子负例矩阵，
  Qwen 小模型 cold/warm、文本、取消、续接及有限恢复均通过同源本地验收。
- **SC-006**: **No Legacy Default Execution**. migration inventory 所有默认生产调用方已迁移；
  阻断旧 Python coordinator/provider/helpers 时同一组正式用例仍可完成。
- **SC-007**: **Reproducible Native Consumer**. 干净 checkout 产出可安装库，
  独立 C++ 应用无 Python 链接/运行依赖，实验机器取得完整源码与依赖身份。
- **SC-008**: **Truthful Successor Delivery**. 181 状态不被本轮改写；182 每个 FR 有 CD/T/PO
  映射，本地 audit/验收/交付证据齐全才关闭，外部实验不借用本地 PASS。

- **SC-009**: **Reviewable Symbol Contracts**. 每个变更符号有覆盖条目和任务/证明映射，所有非 LOCAL_DETAIL 字段有语义说明；公开 C++/Python 文档及使用示例经实际构建/运行验收。设计阶段只计文档检查，不计编译通过。

- **SC-010**: **Static Review Before Runtime Checks**. 每次unit/integration/MiniNDN执行证据均引用先于该次运行的有效S0报告，hash与范围相符、控制性finding为零；unit→integration→MiniNDN前层验收按约定通过。缺失/失效/越范围报告时在运行前停止。静态PASS不计运行PASS。

- **SC-011**: **No False-Green Completion**. 每单元完成证据含三层S0、风险→实际test/PO映射和S1/final diff报告；关键风险无检错证据保持UNTESTED，必要PO未闭合不勾任务。Static review PASS != Behavior PASS；测试全绿也不能豁免缺失生产行为。T016整体S1完成后才能交付。

## Architecture Invariants

| INV | Invariant | Source / authority | Enforcement |
| --- | --- | --- | --- |
| INV-001 | Core 只拥有 service-neutral 网络/安全机制；DI 调用 Core | constitution I/II；docs/ndnsf-core-app-boundary.md | 构建依赖图、Core diff 审查 |
| INV-002 | 默认 DI 业务算法和状态 owner 为 C++；Python 绑定可选 | 本次用户决定；明确替代 architecture.md 中 Python planning 定位 | 无 Python 运行、旧模块阻断 |
| INV-003 | 策略只提案，sealer 校验；Provider 独立授权执行 | 现有 ACK_CLOSED / ProtectedRuntime | 非法提案、错误 grant、租约变化负例 |
| INV-004 | 一份规范编码定义，多方独立验证；不改变已有 wire 契约 | Spec170/180/181 contracts | 冻结向量、双侧生产解码 |
| INV-005 | 普通角色与可选生成共享准备/执行/取消/清理 | Spec181 FR-015；NativeEpochCoordinator | stateless/stateful 回归 |
| INV-006 | 冷动态装配仍在 Selection 后；模型差异归 adapter | NativeCanonicalOnnxAssembler；现有 recipe | 冷缓存、多不同切分和权限先行 |
| INV-007 | 运行时零 Python，不要求离线训练/导出或构建工具零 Python | 本次用户目标；Waf 为构建工具 | 生产进程树/动态库检查 |
| INV-008 | 设计、实现、定向检查、本地验收、外部实验分离 | constitution V/VII/VIII；Spec181 handoff | audit + 同源证据 |
| INV-009 | 本轮只更新设计技能/182文档和相关上下文；不干扰合并代码修复 | 本次用户授权 | 明确路径 diff 与工作区边界 |

## Code Design Index

D 表示 NDNSF-DistributedInference/cpp/ndnsf-di，P 表示
NDNSF-DistributedInference/ndnsf_distributed_inference，A 表示
NDNSF-DistributedInference/cpp/adapters。仅本表使用前缀缩写，附件列全路径。

| CD | Operation / files / symbols | Purpose | Detail |
| --- | --- | --- | --- |
| CD-001 | ADD D/NativeInferenceClient.{hpp,cpp}, NativeInferenceHandle；REUSE ServiceUser::BeginCollaboration / CommitCollaborationPlan | 完整原生 requester | [Public API](contracts/code-design.md#cd-001-public-api) |
| CD-002 | ADD D/NativePlanning.{hpp,cpp}, NativeModelAdapter；A/qwen/NativeQwenPlanner.{hpp,cpp}, A/yolo/NativeYoloPlanner.{hpp,cpp} | 原生 split/placement、模型差异 | [Strategies](contracts/code-design.md#cd-002-strategies) |
| CD-003 | ADD D/NativePlanSealer.{hpp,cpp}；MODIFY NativeExecutionPlanJson.{hpp,cpp} | 计划/依赖/摘要/投影唯一语义 | [Plan semantics](contracts/code-design.md#cd-003-plan-semantics) |
| CD-004 | ADD D/NativeGrantClient.{hpp,cpp}, NativeArtifactPolicyAuthority.{hpp,cpp}；REUSE NativeGrantVerifier / ProtectedRuntime | 原生授权闭合 | [Grant](contracts/code-design.md#cd-004-grant) |
| CD-005 | MODIFY D/NativeCanonicalOnnxAssembler.{hpp,cpp}；ADD A/onnx/NativeOnnxRecipeAssembler.{hpp,cpp} | 替换 runtime Python 装配 helper | [Assembly](contracts/code-design.md#cd-005-assembly) |
| CD-006 | MODIFY D/NativeStandaloneTokenizer.{hpp,cpp}；ADD A/qwen/NativeTokenizer.{hpp,cpp} | 原生 encode/decode，移除解释器调用 | [Tokenizer](contracts/code-design.md#cd-006-tokenizer) |
| CD-007 | ADD D/NativeConversationCoordinator.{hpp,cpp}；REUSE NativeEpochCoordinator / ConversationStateStore | requester 续接/恢复与 Provider 状态协作 | [Lifecycle](contracts/code-design.md#cd-007-lifecycle) |
| CD-008 | ADD pythonWrapper/src/ndnsf/di_bindings.cpp；MODIFY P/app_sdk/application.py, client.py, provider.py, P/__init__.py, P/app_sdk/__init__.py | Python 绑定同一库 | [Bindings](contracts/code-design.md#cd-008-bindings) |
| CD-009 | MODIFY wscript, examples/wscript, tests/wscript, pythonWrapper/setup.py, pythonWrapper/src/ndnsf/_ndnsf.cpp；ADD pkgconfig template | 独立 DI 库和可选绑定 | [Build](contracts/code-design.md#cd-009-build) |
| CD-010 | ADD examples/DI_NativeRequester.cpp；MODIFY DI_NativeProviderExecutable.cpp、三个 maintained runners；RETIRE P/app_sdk/placement.py 等默认路径 | 切换与旧实现退出 | [Migration](contracts/code-design.md#cd-010-migration) |
| CD-011 | ADD tests/standalone/run-spec182-native-closure.py、受影响 unit/integration tests、fixtures 与 inventory | 真实可检错证明 | [Proof](contracts/proof-design.md) |
| CD-012 | MODIFY docs/architecture.md, docs/NDNSF-DI-runtime-workflow.md, docs/ndnsf-core-app-boundary.md；ADD evidence/development-handoff.md | 将新职责写入维护文档并交付 | [Delivery](plan.md#delivery) |
| CD-013 | ADD D/NativeRequestPreparation.{hpp,cpp}, D/NativeOfferAdmission.{hpp,cpp}；MODIFY planned NativeModelAdapter::inspect/encodeInput/decodeResult | 补齐完整调用前后端与 ACK trust | [Preparation](contracts/runtime-boundaries.md#cd-013-preparation-and-offer-admission) |
| CD-014 | ADD D/NativeInferenceProvider.{hpp,cpp}；MODIFY DI_NativeProviderExecutable.cpp | C++/绑定共用 Provider 注册与生命周期 | [Provider host](contracts/runtime-boundaries.md#cd-014-provider-host-and-binding) |

### Main Call Flow

~~~text
C++ application ─────────────────────────┐
Python caller → optional native binding ├→ NativeInferenceClient::request
                                       └→ native input preparation → Core BeginCollaboration
                                          → ACK_CLOSED → native offer admission / graph inspection → snapshot
                                          → C++ model split + placement strategy
                                          → native artifact ensure → NativePlanSealer + NativeGrantClient
                                          → Core CommitCollaborationPlan
                                          → NativeInferenceProvider / protected preparation
                                          → native ONNX assembly + runner / tokenizer
                                          → Core Response / stream
                                          → native adapter decodeResult → handle → caller
~~~

Python 不成为 flow 中间的 planner、grant authority、每 token callback 或补救子进程。
原生策略 extensibility 和 Python compatibility 是两个不同接口，不提供 Python trampoline。

## Decision Register

| Decision | Selected | Alternative / tradeoff |
| --- | --- | --- |
| Language ownership | 完整 C++ DI，Python 仅兼容 | 延续 Python 控制层可快速试验算法，但不满足独立 C++ 调用 |
| Migration style | 完整行为逐段替换，保留固定向量和最小运行基线 | 一次重写难定位退化；长期双默认路径不能验收 |
| Model variability | 原生 adapters + 策略接口 | 在 Core 分支识别模型会破坏通用框架边界 |
| Artifact preparation | 保留 Selection 后按需装配 | 全部离线预切会改变能力，不作为自动简化 |
| Performance | 不作速度/内存改善承诺 | 无对照测量，不能从语言推断结果 |
| Native dependency choices | O-002/O-003 先完成有界证据再定 ABI/锁 | 猜测 ONNX/tokenizer 原生依赖会制造新的隐藏运行依赖 |

## Assumptions

- 本轮分析以 [baseline](evidence/design-baseline.json) 的 committed/workspace-existing 分类为准，
  不假定未提交代码已交付另一台机器。
- “完整”覆盖迁移清单中的现有生产能力及本 Spec 的 YOLO/Qwen 验收；不要求实现从未支持的新模型。
- 离线 Python 导出和 Python 实验 harness 允许；被测 requester、Providers、授权及运行时依赖不允许 Python。
- 181 修复的 Data wire-size 问题不被语言迁移自动解决，T001 必须核对其最终处置。
- 已合并 Core 的请求级加密/撤销/ControllerVersion/权限刷新与持久状态属于保留基线；DI 工件 grant 独立撤销扩展、独立网络权威服务及多GPU/性能资格不自动扩围。

## Design Readiness

**DRAFT / BLOCK for implementation**。用户目标与职责选择已明确，公开 API/行为和证明框架见附件；
O-001（合并修复基线及181承接）、O-002（ONNX 原生字节契约）、O-003（tokenizer 原生依赖 ABI）
、O-004（完整旧能力/调用方清单）与 O-005（隔离设计可行性）在 [code-design](contracts/code-design.md#open-questions)
中保留有界关闭条件。不得把尚未冻结的叶子接口交给实现者临场补全。

本轮可以完成设计文档交付；它不是 READY_FOR_IMPLEMENTATION 或代码完成。
详细参数/状态、工作单元边界、PO 和自审分别见：
[code-design](contracts/code-design.md)、[proof-design](contracts/proof-design.md)、
[runtime boundaries](contracts/runtime-boundaries.md)、[work-units](contracts/work-units.md)、[plan](plan.md)、[tasks](tasks.md)、
[traceability](traceability.md)、[audit](audit.md)、[checklist](checklists/requirements.md)。

## Static Review Contract

[Pre-test static review](contracts/pre-test-static-review.md)定义S0范围、逐代码路径推理、finding修复复审、阶段入口、证据与失效规则。T002--T014在本任务内先审查再focused运行；T015保留完整系统审查，T016按unit→integration→MiniNDN验收，必要检错证明及S1/final diff完成后交付。首次编译前也进行compile-oriented读码，具体三层/风险/有界循环/S1见SR-010--013。无需为每文件另加审查任务。

## Symbol Documentation Contract

[Symbol design](contracts/symbol-design.md) 定义类/方法/状态/注释/用法；[value contracts](contracts/value-contracts.md) 对照合并源码的12类137字段，逐项解释含义；[coverage inventory](contracts/source-field-coverage.json) 保留机器可查来源。嵌套 schema、原生依赖 ABI、注册/取消接线等未决项必须在T001关闭，禁止跳过到实现。

## Revision History

- Revision 5：按用户评论补三层对抗性审查、5项风险/检错义务、Static review PASS != Behavior PASS、有界复审与失败日志驱动修复；新增FR-019/SC-011/PO-016及测试全绿后的S1和final diff。

- Revision 4：新增FR-018/SC-010/PO-015及S0静态代码审查；每单元在unit/integration前读码对照设计并修复复审，T015整体审查仍先于T016正式验收，补报告身份/范围/失效与具名RED限制。

- Revision 3：依据合并工作区修正基线和承接门；保留新 Core 安全/撤销能力，纠正 CandidateBudget 字段；增加 FR-017/SC-009 及逐符号、字段、注释、用法契约。设计交付不代表迁移已完成。

- Revision 2：完整审计后补 CD-013/014 与 PO-013/014；任务 15→17。
  原 T008--015 改为 T010--017；原 T001--007 不变。历史 revision 1 evidence 不回写。
  修复取消语义、兼容退出/回退、harness-before-audit 及 O-005 自依赖；仍 DRAFT。
  审计证据见 [revision 2 review](evidence/audit-revision2.md)。
