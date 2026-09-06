# Spec182 Authoring Evidence

**Date**: 2026-09-06
**Evidence layer**: source review / design-document validation
**Task**: Spec182 revision 1 authoring (not T001 implementation)
**SourceIdentity**: evidence/design-baseline.json
**ImplementationStatus**: NOT_STARTED
**VerificationStatus**: PASS (document checks only)
**AcceptanceStatus**: NOT_ACCEPTED (runtime feature)
**DesignReadiness**: DRAFT / BLOCK for implementation

## Scope and Actual Work

本轮仅创建 specs/182-native-di-python-bindings/ 下设计文档和 baseline。
没有更改 Core/DI/Python 生产源码、构建配置、Spec181、feature pointer、全局模板或 managed context。
新增文档覆盖用户目标、CD/INV/FLOW、工作单元边界、PO/负例、迁移和外部交付责任。
实际文件清单以本轮 Git diff/status 和 checkpoint 为准；全体实现任务 0/15。

## Source Review

主工作区精确路径的 CodeGraph + 源码已确认：
Core BeginCollaboration/CommitCollaborationPlan；Python _request_v3/PlanSealerV3/策略；
原生 Provider/epoch runtime；生产 ONNX helper 和 tokenizer Python subprocess。
baseline 40 个路径的 hashes/classification 已保留；不声称这些未提交源组成已验证发布。

Spec181 最新 R18 日志的 169 条 Data size 异常与 durable evidence 一致。
该事实用于防止将原生化误称为解决所有故障；未重跑该实验。

## Tooling and Workflow

- Context Mode stats 为 anomaly screen，project/active health 本轮 PASS；不以 totals 推断绑定正确。
- 先前 project relevance 查询为空，采用仓库 authority；CodeGraph 中临时比较副本排除。
- specify CLI 不在 PATH，.specify/templates/spec-template.md 为唯一找到的 spec template，
  按技能结构直接 author；未安装新工具或修改模板。
- 误用 scripts/audit_speckit_structure.py --help 得到 missing-file exit 2；
  随后定位并验证本地 speckit-audit 技能所附的真实结构审计入口。
  这是文档工具路径纠正，不是 runtime/protocol 失败。
- optional agent-context hook skipped：Spec181 仍在执行，不自动激活后续 Spec182。
- 本轮授权仅文档；所有 native build、unit/integration/MiniNDN、SIF/Tiger 命令均未执行。

## Document Validation

实际运行本地 speckit-audit 技能的 audit_speckit_structure.py，参数为
specs/182-native-di-python-bindings --strict --json，exit 0，16 FR / 8 SC / 5 user stories /
15 tasks / 0 complete / 16 FR traced，零 blocker/warning。
第一遍发现缺 US 标签；补齐后通过，未更改验收语义。链接检查另发现一个 T013 anchor
拼写和 ConversationStateStore 头文件位置，已按真实源码修正。
只读 Python 文档校验确认本目录链接/锚点、FR/SC/T 追踪、既有 MODIFY/REUSE 路径和
所有未勾选 task 有效；结果见 [document checks](document-checks.json)。
active pointer hash 未变。工作期间其他持续工作推进 HEAD 并改变 Spec181 spec/tasks；
这些变化保留，不回写旧快照，也不算本轮 authoring 成果；T001 必须刷新。
`git diff --cached --check` exit 0；staged scope 核对为本目录 12 个 ADD，
无 Spec181、生产源码或指针修改。最终 Markdown 链接/锚点检查 80 项通过。
结构 PASS 不等于实施就绪；所有 runtime tests 仍 NOT_RUN。
首次 checkpoint 被仓库 pre-commit 钩子拒绝，原因是文档包含开发助手路径引用。
已移除这些非产品路径和一条助手配置 baseline；保留 40 个产品/规格路径及实际检查结果，
重新执行结构与 staged 检查。不绕过钩子；此事件不属于 Spec181 runtime 失败。

## Design Deviations and Remaining Risks

相对通用 specify：保留活动 pointer=181 是当前用户范围所需的 future-spec 模式；
相对 “No implementation details”：按用户创建的 code-design skill 使用规范性技术附件。
O-001--005 和较大批次细化仍未关闭；详见 audit。没有把 planned API 当作 existing。

**NextAction**: 继续 Spec181；关闭后以 T001 刷新 baseline、关闭叶子设计与依赖，再实现。
