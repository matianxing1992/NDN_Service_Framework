# R10-B58 Shared Skill Async Fixture Lifetime Rule

**Date**: 2026-09-09
**Batch**: R10-B58
**Purpose**: 将 R10-B57 暴露的 detached native fixture 生命周期漏检固化到所有 Spec Kit 入口使用的共享规则和新 Spec 模板。

## Change

Updated the versioned shared workflow in:

- `skills/speckit-code-design/SKILL.md`
- `skills/speckit-code-design/references/batch-quality-gates.md`
- `skills/speckit-code-design/references/review-agent.md`
- `skills/README.md`
- `.specify/templates/spec-template.md`
- `.specify/templates/plan-template.md`
- `.specify/templates/tasks-template.md`

Local agent guidance was aligned in `AGENTS.md`, `CLAUDE.md`, and
`docs/agentic_workflow.md`. The rule requires an explicit fixture owner or join/drain barrier
for external Face/io_context/scheduler/timer/callback dependencies, a destructor-order static
check, and repeated selector coverage. It does not permit changing production close/callback
semantics to accommodate a fixture.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | N/A | Workflow-only rule; no product entry | `rg -n "detached|join/drain|fixture lifetime"` across shared rules/templates | No product caller changed |
| `implementation and wire` | covered | shared `batch-quality-gates.md`, `pre-test-static-review.md`, `review-agent.md` | `rg -n "Face|io_context|destructor|Changed gate" skills/speckit-code-design` | Rule is explicit and references the existing five-lane contract |
| `test/harness/oracle` | covered | `.specify/templates/spec-template.md`, `plan-template.md`, `tasks-template.md` | `rg -n "detached|join/drain|selector" .specify/templates` | New features must name ownership/barrier and repeat the selector |
| `build/source closure` | N/A | No product target or source list changed | `git diff --check`; `verify-spec-kit-sync.py --require-entrypoints --require-personal` | Documentation-only; no native rebuild |
| `migration/evidence` | covered | `skills/README.md`, `AGENTS.md`, `CLAUDE.md`, `docs/agentic_workflow.md` | `rg -n "detached|fixture race" skills/README.md AGENTS.md CLAUDE.md docs/agentic_workflow.md` | Shared and local guidance agree; product migration remains unchanged |

## Review and validation

- Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`; read-only review of
  the complete documentation diff, shared references, templates, and local guidance found no
  actionable P1/P2/P3 issue.
- `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal` passed (`11/11` entrypoints; personal shared skill present).
- `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` passed with
  `errors: []`; product task registry remains `3/17` parent tasks complete and runtime/product
  static status remains unqualified.
- `git diff --check` passed.

## Batch retrospective and closure

- `static`: R10-B57's destructor race showed that generic ownership guidance did not require an
  async fixture lifetime map; the new rule adds that concrete check.
- `compile/link`: not applicable; no product target changed.
- `runtime/test`: the triggering runtime evidence is R10-B57; this batch adds the prevention rule
  and does not claim another runtime result.
- `unobserved`: other asynchronous frameworks, cross-process ownership, and qualification remain
  unobserved.
- `Batch growth decision`: documentation-only members share one governance boundary and were
  closed together; no product responsibilities were absorbed.

`CLOSED_FOR_VALIDATION` for the shared workflow rule. This status means the skill/template
contract is synchronized and validated; it does not change Spec182 product or T016 qualification
status.
