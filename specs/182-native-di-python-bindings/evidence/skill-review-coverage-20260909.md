# R8-SKILL Review Coverage Contract

**Date**: 2026-09-09
**Scope**: Spec Kit shared review contract and the active Spec182 workflow
**Baseline**: `24f9eb72810db464d89de96deffca6b04ddfc586`
**Status**: `CLOSED_FOR_VALIDATION` for the documentation/skill-sync unit; product implementation remains `PARTIAL`

## Change

本轮流程复盘发现，原有批次规则虽然要求五个 coverage lane，但没有规定审查输出的最小
格式，容易出现只写 `No findings`、漏看测试注册或漏看 target/source closure 的情况。
共享 `skills/speckit-code-design/references/review-agent.md` 现在定义了固定的 Minimum
Review Record。当前 Spec 的 FR-018/SC-010、`skills/speckit-code-design/SKILL.md`、`skills/README.md`、
`.specify/templates/plan-template.md` 和 `.specify/templates/tasks-template.md` 均要求
使用该记录，并在批末填写 `static`、`compile/link`、`runtime/test`、`unobserved` 四类
`Batch Retrospective`。

本机 `.agents/skills/` 命令副本同步了四个实际缺口：`speckit-analyze` 检查批次复盘和
Execution Progress；`speckit-taskstoissues` 保留五 lane、Review trace、Closure decision
及复盘字段；`speckit-implement` 将 Minimum Review Record 作为继续任务的前置；
`speckit-constitution` 要求治理修改后核对所有入口副本。副本仍按仓库策略保持未跟踪，
规则权威在版本化 `skills/speckit-code-design/`。

## Coverage matrix

| Lane | Status | Scope and check |
| --- | --- | --- |
| `production entry/callers` | `N/A` | 仅修改 Spec Kit 文档与技能规则；未修改 NDNSF-DI 生产入口。由 `rg` 核对入口责任表。 |
| `implementation and wire` | `covered` | Spec FR-018/SC-010、`skills/speckit-code-design/SKILL.md`、`references/review-agent.md` 与两个模板的交叉引用；`rg` 检查 Minimum Review Record、五 lane 和 `Batch Retrospective`。 |
| `test/harness/oracle` | `N/A` | 没有产品行为或测试 harness 变更；文档自检使用 `validate_design.py` 和 Spec Kit prerequisite。 |
| `build/source closure` | `N/A` | 文档/技能变更不改变 C++ target 或 source closure，因此没有 native build。 |
| `migration/evidence` | `covered` | 当前 Spec 的 R8-SKILL 计划/任务记录、本 evidence，以及本机四个入口副本的 SHA-256 已核对。 |

## Validation

- `git diff --check`：exit `0`。
- `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`：`ok=true`，任务/执行单元与链接检查通过；既有 `DONE/PARTIAL/NOT_STARTED` 状态未改变。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks`：exit `0`。
- 只读审查按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 执行；SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`；覆盖本次全部 tracked diff 与四个本机入口副本，`No findings`。
- 文档单元没有 C++ 编译、运行时或 MiniNDN 验收；`-j4` 不适用，不能从本记录推导性能提升或 Spec182 完成。

## Batch Retrospective

- `static`: 原缺口是审查记录格式不固定，已由 Minimum Review Record 收口；本次静态复核未发现新的控制性问题。
- `compile/link`: `none`；没有源码、target 或链接变化。
- `runtime/test`: `none`；没有产品运行或 harness 行为变化。
- `unobserved`: 未来安装副本可能漂移，需在治理修改后重新核对 SHA-256；本单元不声称替代 Spec182 的 C++/跨进程/资格验收。
- `batch expansion`: 未在形成稳定出口后吸收产品职责；本批只覆盖共享规则、模板和入口同步。

## Remaining

R8-SKILL 只关闭公共审查记录格式。Spec182 的生产 requester、maintained caller migration、
跨进程/no-Python qualification 和 T016 仍按 [tasks.md](../tasks.md) 保持 `PARTIAL`，不得
因为技能同步或文档校验通过而勾选产品任务。

## Retrospective Feedback Loop

本次复盘把已有批次的漏检按首个边界归类，并将反馈规则提升到共享 skill/template：

| Category | Existing observation | Required follow-up |
| --- | --- | --- |
| `static` | CC-3B 曾提前发现锁顺序、scope key 清理、replacement 状态和 token 前缀一致性问题；静态门本身有效但不能覆盖全部运行假设 | 继续使用五 lane matrix，并把测试实现、真实 caller/default wiring 和构建注册/source closure 纳入同一只读审查 |
| `compile/link` | R3-B1/R4-B4 批次曾在编译或链接阶段暴露缺参数、缺头文件和 integration source 注册遗漏 | 重试前链接原始日志并登记新增的注册/closure 检查；只重跑原命令不算修复 |
| `runtime/test` | R4-B4/R6-B9 的 freshness 默认值、D2h callback/allocator 边界，以及 R9-B1 的 selection-status 并发 UAF 只能在运行或 ASAN 观测 | 在下一批覆盖矩阵中写明新增 caller/harness/oracle 或并发回归，并保留失败目录；局部通过仍保持 `PARTIAL` |
| `unobserved` | 默认 requester 完整生产链、跨进程 stream/conversation 和 T016 资格尚未观测 | 继续列为 `unobserved`/`gap`，不由构建耗时、任务数或 `STATIC_PASS` 推断完成 |

若同类漏检再次发生，下一批开始前必须修订共享 skill、模板或 checklist，或者在证据中
写出替代门禁及理由。批次耗时只有在 target/source closure、toolchain、配置和工作树可比时
才能作为观测；本记录不支持总体效率百分比。

## Review trace

- **Skill**: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
- **Baseline**: `c3fd5a3a`；**Diff scope**: `skills/speckit-code-design/{SKILL.md,references/batch-quality-gates.md,references/pre-test-static-review.md}`, `skills/README.md`, `.specify/templates/{plan-template.md,tasks-template.md}`, and the active Spec182 `spec.md`, `plan.md`, `tasks.md`, and this evidence record.
- **Coverage queries**: `rg` over the eleven batch-aware `.agents/skills/speckit-*/SKILL.md` entry copies (the separate `speckit-agent-context-update` utility is not a batch executor) for the shared reference/header; `git diff --check` on the scoped paths; `validate_design.py`; prerequisite JSON check; SHA-256 comparison of the three versioned code-design files with `/home/tianxing/.codex/skills/speckit-code-design/`.
- **Findings / re-review**: No actionable findings. The five lanes remain explicit; this amendment adds the miss-feedback rule without changing native behavior, task status, or qualification claims.

## Closure decision

`CLOSED_FOR_VALIDATION` for the workflow/documentation unit. The stable exit is the shared
miss-feedback contract being present in the versioned skill, both templates, and the active
Spec record, with synchronized personal code-design references. A future compile/link or
runtime/test miss must link its first boundary and changed check; repeated misses must trigger
another skill/template/checklist revision or an explicit substitute gate. Spec182 production
requester, maintained caller migration, cross-process/no-Python execution, and T016 remain
`PARTIAL`/`NOT_RUN` as recorded in `tasks.md`.
