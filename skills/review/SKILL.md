---
name: review
description: Review changes from an explicit Git baseline along separate repository-standards and specification axes, including uncommitted work when requested.
---

# Two-Axis Review

## Repository And Change Boundary

在目标 checkout 用 `git rev-parse --show-toplevel` 确定 repo root。
以下仓库路径从该 root 解析；安装本 skill 后仍以目标仓库为准。
读 `AGENTS.md`、`git status` 和当前用户范围，保留已有改动。
用户给定 commit/branch/tag 即为基线；缺少且无法从当前明确任务得出时才问。

先固定实际差异，不把未提交实现遗漏在提交比较之外：

- 已提交分支差异：`git diff <base>...HEAD`，另记 `git log <base>..HEAD --oneline`。
- 用户要求包括当前工作区：`git diff <base> -- <explicit paths>`；它涵盖 tracked staged/unstaged，仍需从 status读取相关 untracked 文件。
- 进行中的 merge：记录 HEAD/MERGE_HEAD，按实际工作树对各方基线比较；three-dot HEAD 不包含尚未提交的 merge结果。

## Sources

Spec 来自用户给定路径、issue/PRD或当前 `.specify/feature.json` 的维护文档。
核对 spec、plan、tasks、契约与最新 evidence；不把历史进度或 planned API 当 existing。
没有规范来源则明确 Spec轴缺少依据，不能自行编造需求。
标准来源优先 `AGENTS.md`、constitution、架构文档、ADR 和贡献约定。
已有 CodeGraph 索引时先用它定位源码；没有索引直接精确读源，不新建索引。

## Separate Review Axes

需要独立审查且当前任务允许 agent协作时，将两个有界只读任务并行分配：
每个任务提供固定基线、实际 diff 命令、精确范围和规范路径。
若当前指令不允许委派，则在本次审查中分别完成并明确这一限制；不以工具缺失阻止可完成的审查。

| Axis | Required judgment |
| --- | --- |
| Standards | 实际改动是否违背仓库规则；引用规则和源码位置，区分硬性违反与判断建议。跳过工具已覆盖的纯格式问题。 |
| Spec | 需求是否缺失/部分实现、实现是否错误、有无越界；逐项引用要求和实际源码。检查调用链、生命周期、权限/版本/deadline，以及测试与collector的独立判据。 |

报告具体可行动发现，区分确认缺陷和待验证疑点；不要为追求数量添加风格项。
优先核对重要成功/失败路径，不把 mock、测试数、任意退出或超时当成真实验收。
每轴报告保持简短（通常400词以内），分别输出 `Standards` / `Spec`，保留严重度、路径和依据。
说明审查范围、未覆盖部分及下一步；审查 PASS 不等于运行 PASS。

默认只读。用户授权修复时才编辑，按文件划分所有权并保留他人改动；完成后只复审受影响范围。
依仓库要求把真实结论同步任务/evidence，是否构建、测试或提交继续受当前用户范围控制。
