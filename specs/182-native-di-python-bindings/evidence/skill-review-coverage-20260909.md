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
