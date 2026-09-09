# R10-B44 Spec Kit Entrypoint Preflight Rule — 2026-09-09

## Scope and stable exit

本批只补共享 `speckit-code-design` 入口规则：新建或更新 Spec Kit feature 前，执行统一的
同步检查，确认版本化模板、本机入口副本和个人安装副本没有漂移。该批不修改 NDNSF-DI
运行时、请求 wire、测试资格或父任务状态。

## Coverage matrix

| Lane | Status | Actual scope / check |
| --- | --- | --- |
| production entry/callers | covered | `skills/speckit-code-design/SKILL.md` 的 feature 创建/更新入口；11 个 `.agents/skills/speckit-*` 副本由 `verify-spec-kit-sync.py` 检查 |
| implementation and wire | covered | `SKILL.md` 新增同步门说明；脚本继续核对三份模板和共享 reference 的 SHA-256 |
| test/harness/oracle | covered | `verify-spec-kit-sync.py --require-entrypoints --require-personal`；过期副本/缺失安装探针沿用 R10-B43 |
| build/source closure | N/A | 仅文档和 Python checker，无 native target 或 ABI 变化 |
| migration/evidence | covered | 个人 `/home/tianxing/.codex/skills/speckit-code-design/SKILL.md` 已同步；本证据与 tasks/plan 互链 |

## Review trace and findings

按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）及项目
`skills/speckit-code-design/references/review-agent.md` 执行只读五 lane 审查。基线为
`6c5dd1cd`，范围为 `skills/speckit-code-design/SKILL.md` 和同步后的个人副本。未发现
P1/P2/P3；规则明确同步检查失败时记录 workflow `gap`，且不把同步结果写成产品 PASS。

## Verification

```text
python3 -m py_compile skills/speckit-code-design/scripts/verify-spec-kit-sync.py  # PASS
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal  # PASS: 11/11; personal present
git diff --check  # PASS
python3 specs/182-native-di-python-bindings/checklists/validate_design.py  # PASS (after tasks/plan update)
```

## Batch retrospective

- **static**: 本批新增入口同步要求；未发现未解释的覆盖缺口。
- **compile/link**: N/A；无产品 target。
- **runtime/test**: checker 正常路径通过；未执行 native request 或资格运行。
- **unobserved**: skill 安装到其他机器、未来 feature 的实际执行仍需各自运行 checker。

## Closure decision

`CLOSED_FOR_VALIDATION` 仅适用于 Spec Kit workflow preflight；产品任务、跨进程请求、
maintained caller/no-Python 和 T016 仍为 `PARTIAL`/`UNQUALIFIED`，不能由本批放行。

