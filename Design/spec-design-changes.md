# Spec 设计变更记录

本文件回答“哪个 Spec，为什么，把哪个模块的什么设计从什么改成了什么，代码实现到哪里”。
这里记录设计影响；Spec 的 tasks.md 与契约继续负责具体任务和验收。CHANGELOG.md 记录文档版本，两者互相引用。

## 记录规则

1. 从本基线起，每个新 Spec 必须登记一条；旧 Spec 后续继续修改设计时也登记。没有设计变化写“无设计变化”并说明依据，不留空白让读者猜测。
2. Spec 建立时记录目标与受影响章节；目标调整时补充前后差异；实施和验收时分别更新实现状态与证据。状态使用 PLANNED、PARTIAL、VERIFIED、NO_DESIGN_CHANGE，不以 Spec 编号或代码提交代替验收。
3. 当前设计只写已核对的实际行为；未实现的目标留在目标设计。部分实现分别列出已接通和未接通部分。
4. 每条保存 Spec 路径、任务/契约 ID、模块、章节标题、变更前后行为、源码提交、文档提交定位和证据。章节标题为主要定位，页码仅辅助。
5. 源码提交使用完整哈希或明确范围；工作树快照必须附逐文件摘要和补丁。文档提交由 `git log -- Design/spec-design-changes.md` 定位，避免在提交内填写自身哈希。
6. 更新两份 PDF、正文、记录和必要的 Spec 进度，同一文档单元核对后提交。回退或取代某项设计时追加记录，不删除历史条目。

## 索引

### 2026-09-12 — Spec185 Prepared Model Runtime

- **2026-09-12 21:07 -05:00 / T003+T004 B2 `VERIFIED`**: C-02 preparation design is now implemented for the bounded local Runtime path. Before, `User::prepare` had no verified Package/cache implementation; after, `ModelPreparationCache` owns canonical source inspection, independent graph identity, immutable `PreparedModelPackage`, single-flight/refresh generations, waiter cancellation/deadlines, leases, LRU/byte budget and exactly-once completion. Core `OperationRuntime` remains the generic owner and has no DI dependency. Source range is the frozen B2 snapshot from base `9bdde3cf` with tracked patch SHA `2d1f7772efef5a0cdc689e340599a2b752262abb673de698427eda3b52ff7d9a`; bounded C++ normal/TSan evidence is recorded in [B2 evidence](../specs/185-prepared-model-runtime/evidence/b2-preparation.md). Request, conversation, Provider, Python and cross-process qualification remain `PLANNED`.

- **2026-09-12 17:00 -05:00 / NO_DESIGN_CHANGE**: 逐任务静态门改为按依赖阻塞，允许单主会话编码与独立快照审查重叠；不改产品API、任务依赖或验收，双PDF无需重建。[验证](../specs/185-prepared-model-runtime/evidence/dependency-scoped-dispatch-20260912.md)。

- **2026-09-12 16:24 -05:00 / NO_DESIGN_CHANGE**: tasks Updated及新checkpoint要求分钟与UTC offset；仅进度元数据格式修订，不改API/行为/状态或双PDF。[验证](../specs/185-prepared-model-runtime/evidence/progress-timestamps-20260912.md)。

- **NO_DESIGN_CHANGE / batch execution**: [批次执行表](../specs/185-prepared-model-runtime/batch-execution.md)补齐18任务12批的静态门/共享验证范围，任务卡改为实际顺序；API、owner及产品行为沿C-01–C-09，本轮不改TeX或双PDF。[证据](../specs/185-prepared-model-runtime/evidence/batch-execution-20260912.md)。

- **Core/App revision**: [C-09](../specs/185-prepared-model-runtime/contracts/core-app-boundary.md)确认协议/流/注册已有Core机制，通用运行时/等待/订阅仍需提取。新增T017/T018前置B0C，现18任务12批；模型/会话/KV语义保留DI。PLANNED，源码未迁移。[证据](../specs/185-prepared-model-runtime/evidence/core-boundary-20260912.md)。

- **Implementation design**: [C-08](../specs/185-prepared-model-runtime/contracts/code-design.md)及逐任务Design binding补齐内部类/函数/字段/流程。T016前移到B1后、T003前；具体路径/完整类型/策略来源/continuation/重载问题已修订。[证据](../specs/185-prepared-model-runtime/evidence/implementation-design-20260912.md)。产品仍PLANNED。

- **T016/B2E implementation and validation**: C-08 `CD08/CD09`, `F16`,
  `FN08`, `FLOW03`, and `PO08` are now implemented in the DI planning,
  adapter, catalog, placement, and runner boundaries. Cooperative strategy
  ports carry `ExtensionControl`; registries use explicit replace-before-freeze
  and read-only lookup; the legacy vtable remains advanced compatibility. The
  normal and clang/TSan C++ selectors plus the installed extension consumer
  pass; full packaging and later request/preparation exits remain unobserved.
  Source/evidence: T016 working-tree range from B1 base `76e656c4`,
  [B2E evidence](../specs/185-prepared-model-runtime/evidence/b2e-extensions.md),
  and [tasks](../specs/185-prepared-model-runtime/tasks.md). Status:
  `VERIFIED` for the bounded B2E exit; Spec185 remains `PLANNED` pending B2–B9.

- **Complete API revision**: [C-07](../specs/185-prepared-model-runtime/contracts/api-catalog.md)统一64组稳定入口、Python直接绑定/便利映射与值/生命周期；Subscription及局部异步等待、read取消、析构和关闭边界归原任务owner。[验证记录](../specs/185-prepared-model-runtime/evidence/api-lifecycle-20260912.md)。PLANNED，不是产品完成。

- **API revision**: [全表面审计](../specs/185-prepared-model-runtime/api-review.md)、[C-05](../specs/185-prepared-model-runtime/contracts/api-usability.md)、[C-06](../specs/185-prepared-model-runtime/contracts/cpp-first.md)。独立C++ SDK及原生异步/Provider入口，Python仅包装；任务扩为16项11批，T013完整C++资格先于T012。
- **Revision evidence**: [API修订记录](../specs/185-prepared-model-runtime/evidence/api-review-20260912.md)；源码未改，当前/冻结目标API快照不覆盖。

- **Status**: PLANNED；[Spec185](../specs/185-prepared-model-runtime/spec.md)、[任务](../specs/185-prepared-model-runtime/tasks.md)。
- **Before/after**: 已有 NativeRequestCatalog 在构造期验证冻结模型，请求期仍需model/splitter等接线；目标提供Runtime/User/PreparedModel和有界准备缓存，NativeInferenceClient继续唯一执行。
- **Contracts**: C-01公开API、C-02缓存身份、C-03请求/会话/Provider、C-04验收；详见[审计](../specs/185-prepared-model-runtime/audit.md)。
- **Design/API**: 目标roadmap新增Spec185章节；新增签名在独立中文契约，不写入已实现API inventory。当前PDF提示历史基线漂移54文件；未刷新并行源码事实。
- **Source/evidence**: 源码审计基线575b43cc93bbed29932303caf3d09974f1585af7；[规划证据](../specs/185-prepared-model-runtime/evidence/planning-20260912.md)。本轮没有native行为或资格测试，184未完成项不变。

### 2026-09-12 — Spec184 portable MiniNDN environment inputs

- **Status**: PARTIAL（environment-input unit `CLOSED_FOR_VALIDATION`）；T007 qualification remains IN_PROGRESS / PARTIAL。
- **Before/after**: MiniNDN runner 的 topology、Provider node、MiniNDN root、content store、app state 和 native executable 依赖分散在默认值与硬编码中；现在由 `ndnsf-di-minindn-environment-v1` profile 提供机器差异，显式 CLI 覆盖，启动前完成节点/binary preflight，并记录解析 profile identity。协议、ACK/Selection、Provider assembly 和 C++ API/wire 未改变。
- **Source / evidence**: source checkpoints `689cca00d40b1e2bf241aff4e7d14b0204a7c892`, `90dc8785` and `15c8abb1`; [Spec184 profile evidence](../specs/184-native-di-closure/evidence/minindn-environment-profile-20260912.md)、[environment profile guide](../docs/ndnsf-di-minindn-environment.md)。
- **PDF boundary**: no current/target API or architecture change; PDFs were not regenerated. Actual remote/Tiger and Qwen3.6-27B evidence remains external。

### 2026-09-12 — Spec184 Native MiniNDN requester route

- **Status**: PARTIAL（caller route `CLOSED_FOR_VALIDATION`）；T007 qualification remains IN_PROGRESS / PARTIAL。
- **Before/after**: MiniNDN runner previously appended `--native-cpu-provider` even when
  `--native-requester-config` was present, making the legacy per-token diagnostic branch win over
  `APPClient.request_native_reference`. The runner now selects mutually exclusive native requester
  and compatibility diagnostic arguments; User rejects the conflicting pair and normalizes the native
  final `tokenIds` response for the existing first-token oracle.
- **Current C++ boundary**: `NativeInferenceClient` still plans after `ACK_CLOSED`; the Provider keeps
  metadata-only startup slots and assembles canonical ONNX after authenticated Selection. No C++ API,
  wire contract, or Provider state-machine change was made.
- **Source / evidence**: checkpoint `7488ac08`; [Spec184 caller evidence](../specs/184-native-di-closure/evidence/native-minindn-post-ack-routing-20260912.md),
  [caller matrix](../specs/184-native-di-closure/contracts/caller-matrix.md), and [tasks](../specs/184-native-di-closure/tasks.md)。
- **PDF boundary**: no Design/API signature or target/current architecture change; current/target PDFs
  were not regenerated. The caller route and qualification limits are recorded in the active Spec evidence.

### 2026-09-11 — Spec184 Native DI Closure / Spec182 Transfer

- **Status**: NO_DESIGN_CHANGE；184 implementation NOT_STARTED；182 TRANSFERRED / qualification INCOMPLETE。
- **Before/after**: 将182剩余14个父任务、四项源码 finding 与两类闭合缺口迁入184，保留已完成实现和原始证据；
  不改变 API/wire、架构目标或当前行为，不复制历史时间线。
- **Baseline**: `94c1e644`；[184 spec](../specs/184-native-di-closure/spec.md)、
  [transfer matrix](../specs/184-native-di-closure/contracts/transfer-matrix.md)、
  [migration evidence](../specs/184-native-di-closure/evidence/migration-20260911.md)。
- **PDF boundary**: 仅文档治理/执行归属变化，双 PDF 与独立源码快照不重新生成；后续修复仍按 MANAGEMENT.md 同步。

### 2026-09-11 — Spec182 Request Chain Audit / R12 Replan

- **Status**: NO_DESIGN_CHANGE（产品实现 PARTIAL）；模块 NDNSF-DI，关联 T005/T010/T011/T013–T017。
- **Source baseline**: `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a`。本轮不修改产品源码、API 或冻结设计快照。
- **Before/after**: 原执行计划累积多个旧 dispatch；现按当前请求链审计的四项缺陷和两类缺口，统一为
  R12-A–E。修复线程、既有会话提交契约和导出行为是后续 PLANNED 工作，不写为当前已修复行为。
- **Evidence**: [静态审计](../specs/182-native-di-python-bindings/evidence/request-chain-static-audit-20260911.md)、
  [R12 调度](../specs/182-native-di-python-bindings/contracts/audit-driven-execution.md)、[tasks](../specs/182-native-di-python-bindings/tasks.md)。
- **PDF boundary**: 没有 API/目标设计变更，不重新生成双 PDF，不覆盖独立冻结的当前/目标快照；
  修复实施时仍须按 MANAGEMENT.md 同步实际行为与源码摘要。本次文档 checkpoint 由 Git 历史定位。

## Spec182：Native-First Execution（2026-09-10）

- 状态 PLANNED；模块 DI；TG-02 增补独立 artifact authority 和 N1–N5 顺序。原先允许生产 requester 内置 issuer、将 process tests 统一留给 T016 的规则被取代。
- R11-B1 至 B9 与 T005/T009–T017 映射见 [执行契约](../specs/182-native-di-python-bindings/contracts/native-first-execution.md)；任务状态见 [tasks.md](../specs/182-native-di-python-bindings/tasks.md)。
- 本轮无源码/API 签名变更；当前快照和冻结目标 API 均保留。目标新增边界仍待实现，不宣称当前 requester 已移除 authority 私钥。
- 核对起点源码提交 `89932fb5b91f488eec6d4dc8756abba3fdaa40bd`；文档提交由本文件 Git 历史定位；验证见 [本轮证据](../specs/182-native-di-python-bindings/evidence/native-first-replan-20260910.md)。

## D-005：四模块图解（2026-09-08）

- 工作单元 D-DESIGN-DIAGRAMS；用户授权补充模块图、类图、时序/状态图。
- 新增图解章节与 G1--G9；API/实现行为无新增，属于现有设计的图形说明。
- 当前 G7 对照 NativeInferenceClient::dispatchOperation 的未就绪终点；目标 G7 仅表达 TG-02/TG-03，PLANNED，不声明完整原生链已实现。
- 当前源码身份与 API 清单同步，目标快照保持冻结；共享对象图核对两侧关系。
- 文档与渲染证据见 [diagram evidence](../specs/182-native-di-python-bindings/evidence/design-diagrams-20260908.md)；状态以该记录为准，不关闭 Spec182 产品任务。

## D-004：逐章修订与关键 API 行为（2026-09-08）

- 工作单元 D-DESIGN-R3；依据用户授权审计并修正 Design。
- 前：第 53 章只有 cancel 声明；grant/流/目录存在错述；BC 重复；目标正文含 R0 过时声明。
- 后：23 组 API 先解释行为，再列带所属符号的准确声明；补字段、状态表、返回/错误及调用示意。
  第 53 章覆盖 epoch/Qwen/KV/journal；BC 合入 AC，当前/目标 58/63 章；五项目标补兼容和验收。
- 新发现：Drone Execute handler 直接转交 backend，未见独立 lease/readiness 重验；
  Qwen cancel 与 handle cancel 不同；journal abort/耐久性限制按实际代码记录，目标显式承接。
- 工具：修复长方法名断行，生成两侧可读 Markdown 声明并逐字节检查，目标使用冻结 inventory。
  维护规则加入原章勘误、逐章阅读和示意/运行证据区分；不把计数当语义通过。
- 当前采样身份见 source-baseline.json 与精确补丁；目标身份继续保留独立冻结记录。
  当前新增模型/候选字段仅写当前契约，未修改目标基线或产品代码。
- 逐项位置：[67 个原主题的修订记录](reviews/chapter-revision-r3-20260908.md)。
- 验证及状态：[R3 evidence](../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)。
  文档修订不关闭 Spec182 产品任务；未定稿目标继续 PLANNED。

## D-003：逐章可理解性审阅（2026-09-08）

- 工作单元 D-DESIGN-CHAPTER-AUDIT；[审阅清单](reviews/chapter-audit-20260908.md)、
  [证据](../specs/182-native-di-python-bindings/evidence/design-chapter-audit-20260908.md)。
- 当前/目标 129 个章节位置按 67 个主题逐项阅读：KEEP 7、EXPAND 36、REWRITE 20、CORRECT 4。
- 第 53 章只展开 cancel，生成/KV/会话方法和流程缺失；第 59 章外部 grant 参数与内部 ABE
  完整策略物化混淆；目标历史说明与 TG 章节冲突。报告区分内容错误、完整性和可理解性。
- 审阅工作完成；文档内容 NEEDS_REVISION。未改产品 API、目标决策、源码、快照和 PDF。
  D-002 的技术检查为历史事实，不解释为逐章语义验收；后续修订按本清单收敛。

R2 新增 D-002（文档校验与行为补充）及 TG-01 至 TG-05（PLANNED）。目标批准来自用户
“先修复设计基线和校验机制，再补关键 API 行为契约，最后将架构改进逐项纳入目标设计”。

## D-002：基线、校验与行为契约

- 工作单元：Spec182 D-DESIGN-R2；[证据](../specs/182-native-di-python-bindings/evidence/design-r2-20260907.md)。
- 变更前：350 文件快照遗漏关键 .cpp；目标渲染共享当前 API；PDF 未绑定全部生成输入。
- 变更后：460 文件实现/配置快照；独立目标 API/源码基线；输入/PDF 构建身份验证；
  全函数 API ID 的保守覆盖状态；新增文件、源码漂移与过期生成内容均检查。
- API 行为：AC-13 修正精确 lookup；BC-01 至 BC-04 补授权失效、句柄异步行为、Repo 和 UAV 边界。
- 当前源码：ca585ab5365189203325a8462d8726d2ea32c98f 加 source-baseline.json 登记的工作区补丁；
  未提交实现只作为字节基线，不随文档暂存，不因此获得产品资格。
- 目标源码：保留 e9fe33994a6ca3ff81893591bd24c3fae43f933f 的 R1 冻结 API/源码快照。
- 文档状态和精确检查结果见证据；剩余 SIGNATURE_ONLY 项不计行为审查完成。

## R2 Target Changes

| ID | 模块 / 目标章节 | 前后变化 | API / 兼容边界 | 状态 / 后续验收 |
|---|---|---|---|---|
| TG-01 | Core / 授权版本与影响范围 | 全局版本失效 → 按权限和密钥变化区分影响 | grant/revoke/getPolicyStatus/install；旧客户端保守处理，线格式待 Spec | PLANNED；撤销、乱序、离线及无关节点刷新证据 |
| TG-02 | DI / 原生请求链 | Python/native 分担运行状态 → 原生唯一状态所有者 | APPClient/NativeInferenceClient/Handle；保留签名和错误兼容，关联 Spec182 原任务 | PLANNED；真实 requester 路径、oracle、失败及取消 |
| TG-03 | 四模块 / 异步 API | 分散描述 → 显式线程、deadline、取消、终态和背压契约 | streaming/handle/transfer/mission；逐 API 迁移 | PLANNED；竞争、重入、资源释放和远端取消 |
| TG-04 | Repo / 能力与恢复 | 模糊跨层能力 → 明确能力表和操作幂等/恢复 | lookup 精确语义不变，组合查询单独契约；格式版本迁移 | PLANNED；崩溃、重复提交、目录/数据不一致 |
| TG-05 | UAV / 类型和状态机 | Fields 与交织控制 → 类型验证、独立状态机与适配器 | command/sendMavlink/mission/job；兼容适配器明确拒绝非法输入 | PLANNED；Mock、真实飞控、失联和迟到结果分阶段 |

仅 TG-02 关联当前 Spec182 的既有迁移任务；其余尚未分配 Spec，不虚构编号。
这些记录不改变当前产品 API 或任何功能验收门；未来修改须同步对应 Spec 的 plan/contracts/tasks。

| 记录 | Spec / 工作单元 | 模块 | 设计影响 | 状态 |
|---|---|---|---|---|
| D-000 | 四模块设计 R0 建档；Spec182 的 D-DESIGN-R0 文档单元 | Core / UAV / DI / Repo | 建立当前/目标一致的 35 章基线；新增版本管理与追踪规则 | VERIFIED（文档） |
| D-182-BASE | [Spec182](../specs/182-native-di-python-bindings/spec.md) 基线观察 | DI | 记录原生迁移当前边界；尚未逐项追溯该 Spec 全部设计差异 | PARTIAL（历史映射） |
| D-001 | [D-DESIGN-API](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md) | Core / UAV / DI / Repo | 从组件级细化到 API 契约与准确声明，增加 AGENTS 管理要求 | VERIFIED（文档；结果见证据） |
| D-182-CC3B | [Spec182 R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring) | DI | 配置化 native requester 接入会话 owner、认证 receipt、Provider COMMIT/ROLLBACK/FINALIZE 与终态清理；真实跨进程两轮仍待验收 | PARTIAL（实现与局部验证） |

## D-001：API 开发者指南与维护规则

- 依据：用户要求 API 级细节并参考 NFD Developer’s Guide；本次不实现新产品 API。
- 变更前：35 章组件设计，API 主要作为源码定位入口。
- 变更后：增加第 36–58 章 API 契约、295 文件声明参考、完整签名/默认值/类型字段与契约映射；MANAGEMENT.md 和本机 AGENTS.md 规定每个 Spec/API 变化的同步、验证与提交流程。
- 当前/目标：本轮两份一致；目标契约独立维护，允许后续加入明确标识的计划接口。
- 实现状态：文档实现；原生 DI requester 等产品未完成项保持原状态。5738 个函数条目不代表 5738 个接口均已运行验证。
- 源码身份：R1 source-baseline.json 与精确补丁，可从 Git 还原 350 个文件；API inventory 逐文件绑定其中 295 个文件。API ID 列表见 contract-map.json。
- 验证与下一步：[API 指南证据](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md)。后续每个 Spec 按 MANAGEMENT.md 同步，不自动覆盖目标。

## D-000：四模块设计建档

- 日期：2026-09-07。
- 依据：用户要求建立当前/目标设计，并追加要求按 Spec 追踪、完整纳入 Git。
- 章节：两份文档第 1–35 章；本次维护修订涉及“文档范围与源码基线”和“文档维护与后续目标变更”。
- 变更前：没有统一的四模块双份设计基线；首版生成后暂时本地排除。
- 变更后：统一保存两份中文 PDF、独立可编辑正文、模块清单、源码摘要和本文件，全部纳入 Git；初始目标技术正文等于当前正文。
- 运行行为变化：无；本单元不修改产品代码。
- 源码基线：`d7fa9c8edef924a6cf29c12d3793049aa794a6be` 加 [快照补丁](evidence/source-baseline-worktree.patch)，对应 [94 文件摘要](source-baseline.json)。不把工作树快照误写成已提交实现。
- 上述 R0 的摘要/补丁指提交 9c019a17 中的同名文件；当前路径已在 R1 推进，回溯时须从该历史提交读取。
- 验证：[文档验证](validation.md)、[Spec 文档工作单元](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。文档状态 VERIFIED 不代表四模块运行资格已全部通过。

## D-182-BASE：原生 DI 迁移的当前设计边界

- Spec：[182-native-di-python-bindings](../specs/182-native-di-python-bindings/spec.md)；任务依据：[tasks.md](../specs/182-native-di-python-bindings/tasks.md)。
- 章节：第 18–23 章 DI，第 31 章“实现状态与验证口径”。
- 已核对现状：Python 应用规划与 C++ Provider 执行组件并存；原生 NativeInferenceClient dispatch 返回 NATIVE_REQUEST_PIPELINE_NOT_READY，准备/放置/封存组件的存在不等于完整请求链已接通。
- 当前/目标差异：本次用户指定 R0 两份一致，因此没有把 Spec182 尚未完成的迁移目标自动写入目标 PDF；Spec182 的目标仍以其 plan/contracts 为准。
- 历史变更前后与提交范围：尚未完整追溯，不能把 R0 快照当作整个 Spec182 的变更清单。下一次该 Spec 设计更新时新增具体条目，并逐项补齐任务 ID、行为差异、源码提交与验证。
- 状态：PARTIAL 仅指本文件的历史映射；不替代或降低 Spec 自身验收状态。

## D-182-CC3B：会话请求事务接线

- 日期 / Spec / 任务：2026-09-08；Spec182 R4-B4 CC-3B；T011-C 保持 PARTIAL。
- 原设计：`NativeInferenceClient` 仅保存会话 coordinator，公开 stream final 没有 receipt
  收集、Provider promotion 或 durable checkpoint 提交。
- 当前变化：配置了 `NativeRequestRuntime` 的请求在 final 阶段创建 owner turn，验证每个角色的
  receipt，发送加密 COMMIT/ROLLBACK/FINALIZE 控制并等待 canonical commit ACK；coordinator
  在 durable gate 中执行 parent/journal 晋升，取消、deadline 和 replacement 清理有明确边界。
  未配置 runtime 的兼容/组件构造仍保留结构化 `NATIVE_REQUEST_PIPELINE_NOT_READY`。
- 兼容与目标边界：不改变既有 collaboration wire；Provider 仍使用现有 request-scope 加密
  端口。真实两轮、跨进程 control/receipt、恢复和 T016 qualification 未完成，因此不能把局部
  build/test 结果写成完整原生请求链。
- 源码与证据：NativeInferenceClient、NativeConversationCoordinator、NativeProviderHandler；
  当前批次命令、日志和剩余出口见 [R4-B4 证据](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring)。
- Design PDF：当前/目标 PDF 继续保持原设计基线；未将未验收的请求链写入目标行为，下一次设计
  PDF 修订须在真实两轮/恢复验收后同步。
- 状态：PARTIAL；下一步补 C++ integration harness，再执行 T012/T013 与 T015/T016。

## D-184：Spec184 原生 YOLO 请求与候选绑定收敛

- 日期 / Spec / 任务：2026-09-11；[Spec184](../specs/184-native-di-closure/spec.md)；T007-A0/A1/A2。
- 模块 / 当前章节：NDNSF-DI User/Provider request-scope、native ONNX assembler、qualification harness。
- 原设计与变化：请求作用域输入原先可由多个 Provider 共用同一逻辑路径；当前实现为每个 Provider
  建立独立的 key/binding/input name 状态，并在清理时逐项失效。`COMPONENT_SET` assembler
  原先按完整图节点数拒绝子集；当前按声明的 role boundary 与 extracted graph 验证 certified
  subset，同时保留 deterministic node bytes 与 node-index cover 检查。
- 当前实现 / 目标边界：Y-A 单 Provider、Y-B 多 Provider、Y-N 七个拒绝边界已由当前候选的
  C++ 生产路径运行验证；Python 只编排 MiniNDN 和收集证据。Qwen3.6-27B、继承 negative/
  retirement、I05、Python retirement 与 SIF/Tiger 仍未实现或未资格化，不能写入当前行为。
- 兼容性 / 安全：保留现有 collaboration wire；request-scoped input name 增加 Provider
  绑定，防止跨 Provider key/state 复用；ONNX subset 只允许 recipe 声明和图输入/role boundary
  可达的节点，非法 cover 继续拒绝。
- 源码范围：`ServiceUser.hpp/.cpp`、`ServiceProvider.hpp/.cpp`、
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；验证 harness
  为 `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`。
- 验证与证据：[T007 process qualification](../specs/184-native-di-closure/evidence/t007-process-qualification-20260911.md)、
  [remainder audit](../specs/184-native-di-closure/evidence/remainder-audit-20260911.md)、
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/`、
  `.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/`、
  `.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/`；candidate receipt verify exit `0`，
  Y-A/Y-B/Y-N `PASS`，`git diff --check` 和 Python syntax check 通过。
- 状态：`PARTIAL`；本地 C++/YOLO 行关闭为 `PASS_FOR_ROW`，Qwen3.6-27B 明确
  `WAITING_EXTERNAL_INPUT`。下一步仅处理 A4 继承行与外部模型/实验机，不把 0.6B smoke 代替 27B。

## 新记录模板

### D-185-B7R：runner identity 与 exact-forward cache 生命周期契约

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T021 / C-03、C-08。
- 模块 / 当前与目标章节：NDNSF-DI `NativeModelRunner` 与 `ProviderRoleWorker`；当前 API reference 的 `NativeModelRunner` 条目。
- 原设计 / 新设计 / 修改原因：exact-forward cache 原先用 runner 原始地址区分实例；生产两次独立授权请求中 allocator 复用地址会错误命中已销毁 runner 的输出。当前改为进程内外部 registry 分配单调 runner identity，并在 base destructor 移除登记；cache key 使用该 identity。公共多态基类不增加数据成员，保持对象布局不变。
- 当前已实现部分 / 目标未实现部分：normal 与 ASan/UBSan C++ protected Provider selector 观察两次独立 grant 各自 source fetch、assembly、runner creation 和 execution；生产 grant issuance 与真实 ONNX Runtime 模型仍由 fixture/后续资格批次覆盖。
- 兼容性、迁移或撤回影响：新增构造、析构和 identity accessor 已登记到当前 DI API inventory/reference；无 wire 字段变化，旧 runner 地址不再作为 cache identity。
- 源码提交或范围 / 文档提交定位：`NativeModelRunner.hpp/.cpp`、`ProviderRoleWorker.cpp`、T021 C++ selector；本地 checkpoint 待组合门通过后提交。
- 验证命令、结果与持久证据：`.codex-tmp/spec185-t021-runtime/production-independent-grants-normal-v5.log` 与 `production-independent-grants-asan-v3.log` 均 `RC=0`；详见 [B7R T021 evidence](../specs/185-prepared-model-runtime/evidence/b7r-lifecycle-fixes-20260915.md#t021-follow-up-production-protected-independent-grant-matrix)。
- 状态：PASS（T021）；T013、T012、T014 仍按各自验收边界保持未完成。

### D-185-B4：会话追加输入的 native adapter token 契约

- 日期 / Spec / 任务：2026-09-13；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T007/T008 B4。
- 设计变化：取消调用者可写的 `RequestOptions.canonicalTokenIds`，改由已验证 native adapter
  的 `conversationInputTokens(Input)` 生成当前输入 suffix；native `Conversation` 在首轮建立
  token prefix，在 `APPEND_DELTA` 接到 durable parent 后再由 coordinator 校验严格增长。
- 原因与边界：此前公开包装没有把本轮输入的 canonical token 谱系传到 coordinator，第二轮只能在
  `beginTurn` 被正确拒绝；直接增加公开 vector 会允许调用者伪造 lineage，因此改为 adapter-owned
  encoder。该机制不是新的 parent receipt、role map、plan 或 Provider 参数；没有 pinned encoder
  的 adapter 必须显式报 `UNSUPPORTED_CAPABILITY`。
- 源码与契约：`NativePlanning.hpp`、`NativeCatalogModelAdapter.*`、
  `NativeCanonicalPreparationCatalog.*`、`NativeRequestCatalog.cpp`、`PreparedModel.*`、
  `Conversation.hpp/.cpp`、B4 C++ fixture；对应
  `contracts/public-api.md`、`execution.md`、`api-catalog.md`、`code-design.md`、`api-usability.md`。
- 验证边界：旧 B4 r5 失败证据保留于 [b4-conversation](../specs/185-prepared-model-runtime/evidence/b4-conversation.md)；
  新字段仅完成静态复审准备，normal/sanitizer selector 尚未重跑，B4 仍 `PARTIAL`。当前/目标 PDF
  在 B4 通过并进入文档交付时按 MANAGEMENT.md 统一刷新，未把本次未验收行为写成资格 PASS。

### D-185-B8/B9：PreparedModel 原生入口与设计交付快照

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T012/T014 / C-05、C-06、C-07、C-08。
- 模块 / 当前与目标章节：NDNSF-DI `Runtime`、`User`、`PreparedModel`、`NativeInferenceClient`、pybind facade；Design 当前/目标 API、G7 图与双 PDF。
- 设计变化：当前设计从“原生 dispatch 尚未接通”的历史快照更新为区分两条实际路径：`Runtime::open → User::prepare → PreparedModel → NativeInferenceClient` 已由 C++ 过程矩阵和 Python 薄绑定验收；没有 preparation/request contract 的兼容构造仍返回 `NATIVE_REQUEST_PIPELINE_NOT_READY`。目标设计继续独立保留 TG-01--TG-05 的 `PLANNED` 内容。
- 修改原因：T013/T012 已形成当前候选的原生过程与包装边界，旧当前书会把已验证实现误报为未实现；API 清单、绑定映射、行为覆盖和源码快照也需要绑定同一工作树身份。
- 当前已实现 / 目标未实现：B7 C++ 过程矩阵、B8 Python 绑定和 lifecycle 修复有各自证据；subinterpreter、wheel packaging、Spec184 外部模型/retirement、I05 与 SIF/Tiger 仍保持未观测或 `PARTIAL`。
- 兼容性 / 撤回：保留旧 `APPClient` 编排和无 runtime 的兼容失败语义；未新增 wire 字段。若回退文档，只能回退当前设计说明，不得覆盖目标快照或降低已记录的 C++ 资格证据等级。
- 源码范围 / 文档提交定位：`Design/api/*`、`Design/current-api.tex`、`Design/current-content.tex`、`Design/current-design.tex`、`Design/current-diagrams.tex`、`Design/diagrams/di-current-flow.tex`、`Design/api-contracts.json`、`Design/validation.md` 与 `specs/185-prepared-model-runtime/evidence/b9-handoff.md`；源码身份由 `Design/source-baseline.json` 和 `evidence/source-baseline-worktree.patch` 绑定。
- 验证命令、结果与持久证据：`test_design_state.py`、`verify-api-reference.py`、`verify-source-baseline.py` 均 `PASS`；双 PDF 构建和 `verify.py` 均 `PASS`，构建目录 `.codex-tmp/design-pdf-20260915T130303244487Z/`，详见 [B9 handoff](../specs/185-prepared-model-runtime/evidence/b9-handoff.md)。
- 状态：`PASS`（文档交付）；这不升级 Spec184 的外部资格，也不把文档门替代 native C++ 运行验收。

### D-编号：设计变化名称

- 日期 / Spec 链接 / 任务与契约 ID：
- 模块 / 当前与目标章节标题：
- 原设计 / 新设计 / 修改原因：
- 当前已实现部分 / 目标未实现部分：
- 兼容性、迁移或撤回影响：
- 源码提交或范围 / 文档提交定位：
- 验证命令、结果与持久证据：
- 状态 / 剩余验收 / 下一步：

历史 Spec 的完整回溯是后续独立核对工作，本轮不虚构它们的变更记录。
