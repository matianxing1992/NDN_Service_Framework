# R10-B28 Workflow Authority Alignment — 2026-09-09

## Scope

本批修正当前工作流总览与已生效 constitution/Spec Kit 规则之间的冲突。`docs/agentic_workflow.md`
原先把 GSD Core 描述为默认四道强制门；当前权威规则是 Context Mode、CodeGraph 和 Spec Kit
作为默认门，GSD 仅在长时、分阶段或需要其 phase/state 模型时按需使用。`CLAUDE.md` 的本机入口
同步同一边界，并明确所有 Spec Kit 命令共用 `batch-quality-gates.md` 及 native C++ test
ownership。`AGENTS.md` 保留相同 `-j4` 与增量构建规则，并新增同一批次/native-test 执行契约。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `N/A` | 无产品入口；本批只改工作流说明 | `git diff -- docs/agentic_workflow.md CLAUDE.md` | 不涉及 NDNSF-DI runtime；复审确认无源码改动 |
| `implementation and wire` | `N/A` | 无实现、wire 或 schema 变更 | `git diff --check`; `rg` workflow clauses | 不适用；无协议变化 |
| `test/harness/oracle` | `N/A` | 无产品测试；仅文档规则检查 | `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | validator 保持 `errors: []`；不把文档检查写成行为测试 |
| `build/source closure` | `N/A` | 无 native target、source list 或生成物变化 | `git diff --name-only`; `git check-ignore -v AGENTS.md CLAUDE.md docs/agentic_workflow.md` | 无产品构建；入口说明按本机 ignored 文件维护 |
| `migration/evidence` | `covered` | `AGENTS.md`, `CLAUDE.md`, `docs/agentic_workflow.md`, `skills/README.md`, shared `batch-quality-gates.md`, this record | `rg` contradiction scan; shared-reference SHA comparison | GSD 默认门冲突已消除；历史 Spec/GSD evidence 保持原事实 |

## Static findings

- 发现并修复 `docs/agentic_workflow.md` 将 GSD Core列为默认 mandatory gate 的过时表述。
- 发现并修复 `CLAUDE.md` 的 “Mandatory Five-Tool Gate” 标题和 GSD/ARS 段落可能让本机入口
  误解为所有任务都必须启动 GSD/ARS；改为 required gates 加 conditional tools。
- 在本机 `AGENTS.md` 增加同一份 Spec Kit 批次与 native C++ test ownership 摘要，使 canonical
  instructions 直接约束未来执行者；原有 `-j4`/增量构建规则保持不变。
- 将共享批次五 lane、稳定出口、review-agent trace、四类 miss retrospective 以及 native
  C++ fixture/driver/oracle 约束加入总览，保持与 `skills/speckit-code-design`、Spec182
  plan/tasks 和 `AGENTS.md` 一致。

## Compile/build misses

`none`: 本批没有产品代码、构建配置或 native target 变化，因此没有启动构建。

## Runtime/test misses

`none observed`: 本批是工作流文档同步；没有把文档检查、CLI smoke 或既有局部 selector 当作
native behavior、parity 或 qualification 证据。

## Build measurement

`BUILD_NOT_APPLICABLE`: no product build. Checks were `git diff --check`, targeted `rg`,
`validate_design.py`, and versioned/shared-reference consistency checks. No `-j` timing is
reported for this documentation-only batch.

## Review trace

- Profile: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- Profile SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `11dd2db0`
- Diff scope: `AGENTS.md`, `docs/agentic_workflow.md`, `CLAUDE.md`; no product source or test changes.
- Review lanes: the five rows above, plus exact contradiction scan for `GSD` mandatory wording and
  native C++/shared batch clauses; re-review found no remaining contradiction in current guidance.

## Closure decision

`CLOSED_FOR_VALIDATION` for this bounded workflow-document boundary. The independent exit is a
consistent description of required versus optional tools and the shared Spec Kit batch contract.
No product caller, C++ target, runtime behavior, maintained-caller migration, or T016 gate is
closed by this record; those remain `PARTIAL`/`UNQUALIFIED` in the active tasks registry.

## Batch Retrospective

- `static`: found the stale GSD-default wording by comparing the overview with constitution 1.5.0;
  added explicit shared batch/native-test references and rechecked the affected files.
- `compile/link`: `none`; no source or target changed, so no compiler/linker miss was possible.
- `runtime/test`: `none observed`; no product runtime was run and no document result was promoted.
- `unobserved`: whether a future executor actually loads and follows the installed skill remains
  an execution concern; the document cannot prove host dispatch or product qualification.

The batch did not expand after its stable documentation exit. Build timing is not comparable or
reported because there was no product build and the touched workflow files are ignored local
context files; versioned shared skill/template rules remain the repository authority.

## Validation result

`STATIC_PASS` for the documentation boundary only. `QUALIFICATION_PASS` is not applicable and no
Spec182 parent task status changes.
