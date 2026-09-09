# R10-B35 Spec Kit Command Output Contract

**Date**: 2026-09-09
**Baseline**: `6aeca3b5`
**Scope**: workflow documentation and local Spec Kit entry copies only; no product source or native runtime behavior changed.

## Purpose and boundary

R10-B34 的复盘显示，已有共享门禁覆盖五个 lane、漏检分类和批次增长判断，但入口
skill 仍可能只引用规则而不生成结果字段。本批把最小输出契约写入版本化
`skills/speckit-code-design/references/batch-quality-gates.md`，并在 code-design skill、
`skills/README.md` 和本机 Spec Kit 入口副本中明确：编辑前登记 `Batch growth decision`；
审查记录列出真实 caller、测试注册和 source closure；批末同一记录包含四类
`Batch Retrospective`、可比构建测量、`Review trace` 和 `Closure decision`；重试必须有
`Changed gate`。该规则不改变 T004--T017 或 T016 的产品验收依赖。

## Static review trace

使用官方只读 [review-agent](https://github.com/openai/codex/blob/main/codex-rs/skills/src/assets/samples/review-agent/SKILL.md)
（本机 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`），按其要求读取
`AGENTS.md`、完整变更范围和相关模板。版本化变更范围为：

- `skills/speckit-code-design/SKILL.md`
- `skills/speckit-code-design/references/batch-quality-gates.md`
- `skills/README.md`
- `specs/182-native-di-python-bindings/plan.md`
- `specs/182-native-di-python-bindings/tasks.md`
- 本机 `.agents/skills/speckit-{specify,clarify,plan,tasks,analyze,audit,converge,implement,checklist,taskstoissues,constitution}/SKILL.md`

实际检查：

```text
git diff --check
targeted link scan over skills/README.md and code-design references
11 local entry copies contain Command Output Contract and Batch growth decision
sha256sum versioned/personal code-design SKILL.md and batch-quality-gates.md (equal)
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
```

结果：无 P1/P2/P3 回归；共享 reference、入口 skill、模板链接和安装副本均覆盖本批字段。
`validate_design.py` 的产品状态仍如实为 `tasks=17`, `tasks_complete=3`,
`runtime_tests=NOT_RUN`, `product_static_review=NOT_RUN`, `design_readiness=DRAFT`；其余
失败边界属于当前 Spec 的既有未完成验收，不是本批文档缺陷。

## Minimum Review Record

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `N/A` | workflow documentation; no product entry changed | `git diff --name-only 6aeca3b5 -- skills/speckit-code-design skills/README.md specs/182-native-di-python-bindings/plan.md specs/182-native-di-python-bindings/tasks.md` plus scope review | 不适用；本批不改变生产 caller |
| `implementation and wire` | `covered` | `skills/speckit-code-design/SKILL.md`; `references/batch-quality-gates.md`; `skills/README.md` | `rg -n "Command Output Contract|Batch growth decision|Changed gate|Batch Retrospective" skills` | 输出字段和禁止升级条件一致；复审无 actionable finding |
| `test/harness/oracle` | `covered` | active `tasks.md` result record; entry-copy checks; `validate_design.py` | targeted Python check for 11 copies; validator | 文档边界检查通过；无 native selector，保持 `N/A`，不提升产品行为 |
| `build/source closure` | `N/A` | no product target or source list changed | changed-path review; no native build requested | 不适用；本批不声称 `BUILD_PASS` |
| `migration/evidence` | `covered` | `plan.md`, `tasks.md`, this evidence, personal code-design copy | `git diff --check`; SHA-256 pair; link scan | checkpoint、既有 T016/跨进程缺口均保留；复审无 actionable finding |

## Batch result

- **Static findings**: none; the new contract closes an entry-skill output ambiguity.
- **Compile/build misses**: none observed; no product source or target changed.
- **Runtime/test misses**: none observed for this documentation boundary; native request/result,
  maintained caller/no-Python, cross-process transport and T016 were not run and remain open.
- **Build measurement**: `BUILD_NOT_APPLICABLE`; no build was run. Text, link, hash and validator
  checks completed in under one second; this is not a product performance measure.
- **Behavior result**: `STATIC_PASS` and `CLOSED_FOR_VALIDATION` for the workflow-documentation
  boundary only; not `QUALIFICATION_PASS`.
- **Evidence / remaining**: product parents remain `PARTIAL`/`UNQUALIFIED` as recorded in
  [`tasks.md`](../tasks.md). Future retries must name a real `Changed gate`.

### Batch growth decision

The batch had one stable exit: every applicable entry skill emits the shared result fields. After
that exit no product caller, state machine, selector, source closure, or qualification dependency
was added. Any such work starts a new Batch ID.

### Batch Retrospective

- `static`: no new miss; the gap addressed was entry-output ambiguity.
- `compile/link`: not observed; no product build was in scope.
- `runtime/test`: not observed for this documentation-only boundary; product runtime remains open.
- `unobserved`: maintained caller/no-Python, cross-process Provider worker, and T016 remain
  explicitly unobserved and were not promoted by this batch.
