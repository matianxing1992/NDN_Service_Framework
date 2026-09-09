# R10-B27 Native C++ Test Ownership Skill Sync (2026-09-09)

## Scope and stable exit

本批只同步可复用的 Spec Kit 规则，不修改 NDNSF-DI 产品代码，也不重排已有产品任务。
稳定出口是：共享门禁、生成模板和已安装 `speckit-code-design` 副本都明确要求，凡断言
原生 runtime/protocol/state/concurrency/crypto/model 行为的 unit、integration 和
regression 测试，必须由 C++ fixture/driver/oracle 直接调用生产 C++ target；Python
只能编排外部设施、启动 C++ executable，或验证 binding/facade、offline oracle 和配置拒绝。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `N/A` | 本批无产品入口或 caller 变更；规则约束生产 caller 的验收方式 | `rg -n "Native Test Ownership|production C\\+\\+ target" skills/speckit-code-design/SKILL.md skills/speckit-code-design/references/batch-quality-gates.md` | 规则明确 native 行为不得由 Python 主断言代替 |
| `implementation and wire` | `covered` | `skills/speckit-code-design/references/batch-quality-gates.md`; `skills/speckit-code-design/SKILL.md` | `rg -n "fixture/driver|directly call|直接调用生产 C\\+\\+ target" skills/speckit-code-design` | shared reference 与入口技能同步，复审无遗漏 |
| `test/harness/oracle` | `covered` | `.specify/templates/spec-template.md`; `.specify/templates/tasks-template.md` | `rg -n "fixture/driver/oracle|C\\+\\+ target/selector|启动 C\\+\\+" .specify/templates` | 新任务必须声明 C++ 测试主体；Python 仅保留编排/绑定边界 |
| `build/source closure` | `N/A` | 本批不改产品 target 或 Waf source list | `git diff --check`; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | 无产品构建；模板规则要求后续任务登记真实 C++ target/selector |
| `migration/evidence` | `covered` | `skills/README.md`; `/home/tianxing/.codex/skills/speckit-code-design/{SKILL.md,references/batch-quality-gates.md}`; `.agents/skills/speckit-{specify,plan,tasks,implement,analyze,audit,converge}/SKILL.md` | `sha256sum` 比较 versioned/installed shared files；`rg -l "batch-quality-gates" .agents/skills/speckit-*/SKILL.md` | installed shared copy 与 versioned source 同 hash；本机 Spec Kit 入口均引用同一门禁 |

## Validation record

- `git diff --check`：PASS。
- 模板与 shared reference 的关键规则查询：PASS。
- versioned 与 installed `speckit-code-design` 的 SHA-256：PASS；`SKILL.md` 为
  `fb78722051520cff2efd1486d9ecb86325eaba0adc490f1a8934676293e435e8`，
  `batch-quality-gates.md` 为
  `bd34a2ba69a8ae20f9dac8f2480b6d02f7249d6c137bf629c687ba17c73a4874`。
- `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`：PASS（`ok: true`，
  `tasks_complete: 3`，`execution_cards: 40`，`progress_units: 40`）；本批不运行产品 C++ build。

## Review trace and closure

使用 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）复核当前 diff，
范围为 shared reference、code-design skill、README、spec/plan/tasks templates 和本证据。
五 lane 已逐行记录；无控制性发现。

`Closure decision: CLOSED_FOR_VALIDATION` 仅适用于本次 skill/template 同步。它不提升
任何 Python focused test、CLI smoke、已有 C++ 局部 selector 或 T016 资格状态；Spec182
的跨进程、maintained caller、I02--I08、no-Python 和最终 qualification 仍按
`tasks.md` 保持 `PARTIAL`/`NOT_STARTED`。

## Batch Retrospective

- `static`: none after the wording re-review; the rule now names the test implementation
  boundary and the allowed Python roles.
- `compile/link`: not observed; no product target changed or built.
- `runtime/test`: not observed; this is a documentation/skill boundary, not behavior evidence.
- `unobserved`: real caller migration and qualification remain intentionally unobserved and
  stay open in the parent tasks.

本批在稳定出口后未吸收新的产品职责；耗时不作效率结论。
