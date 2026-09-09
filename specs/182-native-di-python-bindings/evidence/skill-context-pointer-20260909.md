# Context Pointer Workflow Skill Boundary — 2026-09-09

## Scope and allocation

本批把上下文指针更新规则落到版本化的共享 Spec Kit skill。`skills/speckit-code-design/SKILL.md`
新增 `Context Pointer Updates`，明确 `speckit-agent-context-update` 只维护托管 plan 指针，
不属于 feature artifact、逻辑批次、产品实现或资格验收；`skills/README.md` 的入口说明和
本机个人安装副本保持一致。没有修改 NDNSF-DI 生产代码、构建目标或运行时行为。

## Batch growth decision

分配依据固定为同一项工作流边界、同一份版本化 skill、同一套 marker/指针/sync 检查和同一个
文档出口。出现产品行为、构建或资格依赖时必须另建批次；本批在 skill 与入口副本同步后停止
扩张。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Result |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `N/A` | 无生产入口或调用方变更 | `git diff --name-only -- skills/speckit-code-design` | 不适用：仅工作流文档 |
| `implementation and wire` | `covered` | `skills/speckit-code-design/SKILL.md` 的 `Context Pointer Updates`；`.agents/skills/speckit-agent-context-update/SKILL.md` | `rg -n "Context Pointer Updates|operational pointer|STATIC_PASS|BUILD_PASS|DONE" skills/speckit-code-design/SKILL.md .agents/skills/speckit-agent-context-update/SKILL.md skills/README.md` | 版本化规则与本机运维入口均限定为指针同步 |
| `test/harness/oracle` | `N/A` | 无产品测试或 oracle 变更 | `rg -n "marker|plan.md|verify-spec-kit-sync" .agents/skills/speckit-agent-context-update/SKILL.md skills/speckit-code-design/SKILL.md` | 仅检查 marker、活动 plan 和 skill 同步，不产生行为证据 |
| `build/source closure` | `N/A` | 无产品 target/source/link 变化 | `git diff --name-only -- skills/speckit-code-design .specify/templates` | 不适用：不构建产品 |
| `migration/evidence` | `covered` | `skills/README.md`；11 个 `.agents/skills/speckit-*` 入口；`/home/tianxing/.codex/skills/speckit-code-design/SKILL.md` | `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal`; `sha256sum` 对照 | 入口和个人共享副本可继续引用同一版本化 contract |

## Review trace

只读审查基线为 `80cfb6fe`，差异范围为版本化 `skills/speckit-code-design/SKILL.md`、
本批 plan/tasks/evidence 和同步副本。按官方 `review-agent` 方法（
`/home/tianxing/.codex/skills/review-agent/SKILL.md`，
SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）检查完整差异，
并核对 `AGENTS.md`、共享 README、入口同步器和 Context Mode 指针条件；没有发现 P1/P2/P3。

## Validation

```text
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal  PASS
git diff --check  PASS
python3 specs/182-native-di-python-bindings/checklists/validate_design.py  PASS (ok=true)
sha256sum versioned/personal code-design skill  MATCH
11 local Spec Kit entrypoints  PASS
```

## Batch retrospective

- `static`: 新规则明确了运维指针与产品验收边界；marker、活动 plan、状态词和单一 contract 引用均已检查。
- `compile/link`: 无产品源代码、target 或链接变化，未执行构建。
- `runtime/test`: 无运行时或产品测试；skill 同步检查不构成行为 PASS。
- `unobserved`: 真实 native request/result、跨进程 transport、maintained caller/no-Python 和 T016 qualification 仍由产品批次观察。

`Closure decision`: `CLOSED_FOR_VALIDATION` 仅针对本工作流文档边界；Spec182 产品任务与资格状态不变，后续产品工作为 `OPEN_FOR_NEXT_BATCH`。
