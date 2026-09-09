# CLI and Harness Evidence Boundary — 2026-09-09

## Scope and allocation

本批只补充共享 Spec Kit 流程对命令级证据的边界。`--help`、usage/schema rejection、
可执行文件存在、target/link smoke 或 harness 启动只能证明命令解析、构建接线或外部
设施边界；没有真实生产请求进入 Core/provider 并得到独立结果时，不能记录 native
request/result、Python/C++ parity 或 qualification PASS。该规则不改变 Spec182 产品契约
和任务状态。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Result |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `N/A` | 文档规则，无生产入口变更 | `git diff --check`; `rg -n "CLI|harness|qualification" skills .specify/templates` | 不适用：只定义证据边界 |
| `implementation and wire` | `covered` | `skills/speckit-code-design/references/batch-quality-gates.md`; `skills/speckit-code-design/SKILL.md` | `rg -n "CLI And Harness Boundary|target/link smoke"` | 共享 reference 与入口已同步 |
| `test/harness/oracle` | `N/A` | 模板示例和结果标签，无测试实现变更 | `rg -n "help|usage/schema|native request/result" .specify/templates` | 不适用：不新增产品测试 |
| `build/source closure` | `N/A` | 无产品 target/source 变化 | `git diff --name-only` | 不适用：文档-only |
| `migration/evidence` | `covered` | `.specify/templates/{plan-template,tasks-template}.md`; `skills/README.md`; installed code-design copy | `sha256sum skills/speckit-code-design/SKILL.md /home/tianxing/.codex/skills/speckit-code-design/SKILL.md` and reference pair | 规则可被后续批次引用，产品资格保持开放 |

## Validation

```text
git diff --check -- skills/speckit-code-design .specify/templates/plan-template.md .specify/templates/tasks-template.md skills/README.md  exit=0
python3 specs/182-native-di-python-bindings/checklists/validate_design.py  ok=true
SHA-256 versioned/installed code-design skill and references  match
```

## Batch retrospective

- `static`: no introduced defect; the new boundary is explicit in the shared reference and templates.
- `compile/link`: none; no product source or target changed.
- `runtime/test`: none; no runtime was claimed or run.
- `unobserved`: actual native request/result, parity, cross-process behavior and T016 qualification remain unobserved and stay `PARTIAL`/`gap` in product batches.

The batch did not expand after its stable documentation exit. `Closure decision`: `CLOSED_FOR_VALIDATION`
for the documentation boundary; product work remains `OPEN_FOR_NEXT_BATCH`.
