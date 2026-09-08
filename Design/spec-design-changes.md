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

| 记录 | Spec / 工作单元 | 模块 | 设计影响 | 状态 |
|---|---|---|---|---|
| D-000 | 四模块设计 R0 建档；Spec182 的 D-DESIGN-R0 文档单元 | Core / UAV / DI / Repo | 建立当前/目标一致的 35 章基线；新增版本管理与追踪规则 | VERIFIED（文档） |
| D-182-BASE | [Spec182](../specs/182-native-di-python-bindings/spec.md) 基线观察 | DI | 记录原生迁移当前边界；尚未逐项追溯该 Spec 全部设计差异 | PARTIAL（历史映射） |
| D-001 | [D-DESIGN-API](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md) | Core / UAV / DI / Repo | 从组件级细化到 API 契约与准确声明，增加 AGENTS 管理要求 | VERIFIED（文档；结果见证据） |

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
