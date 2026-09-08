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

## 新记录模板

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
