# R10-B45 Spec Kit Entrypoint Preflight Enforcement — 2026-09-09

## Scope and stable exit

本批把 R10-B44 的共享规则落实到实际安装的 11 个 Spec Kit 入口。每个会创建或更新
feature artifact 的入口现在都明确要求在编辑前运行同步预检；同步检查器也反向验证这些
入口包含预检命令标记。只负责更新 agent context 指针的入口仍不属于 feature artifact
入口集合。本批不修改 NDNSF-DI 产品代码、测试 target 或资格状态。

稳定出口是强制同步检查在 11/11 本机入口和个人共享副本上通过，且缺少预检标记的入口会
被检查器拒绝；该出口只证明 Spec Kit 工作流接线，不证明产品行为或资格验收。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `.agents/skills/speckit-{specify,clarify,plan,tasks,analyze,audit,implement,converge,checklist,constitution,taskstoissues}/SKILL.md` | `rg -n 'Feature Sync Preflight|verify-spec-kit-sync.py --require-entrypoints' .agents/skills/speckit-*/SKILL.md` | 11 feature-facing entries contain the preflight; `speckit-agent-context-update` remains intentionally excluded |
| `implementation and wire` | `covered` | `skills/README.md`; `skills/speckit-code-design/scripts/verify-spec-kit-sync.py` (`ENTRYPOINT_MARKER_GROUPS`) | `rg -n 'Feature Sync Preflight|--require-entrypoints' skills/README.md skills/speckit-code-design/scripts/verify-spec-kit-sync.py` | README states the policy; checker enforces both command and flag markers |
| `test/harness/oracle` | `covered` | checker marker groups and local entrypoint copies | normal forced run; removed-marker temporary copy returns exit 1; normal run returns exit 0 | checker now fails closed for a stale entrypoint instead of relying on a manual review list |
| `build/source closure` | `N/A` | documentation and Python checker only; no native target or generated source | `python3 -m py_compile skills/speckit-code-design/scripts/verify-spec-kit-sync.py` | no product build is applicable |
| `migration/evidence` | `covered` | versioned shared README/checker, local `.agents` entrypoints, personal shared skill | `sha256sum` shared files; `verify-spec-kit-sync.py --require-entrypoints --require-personal`; validator and `git diff --check` | local entrypoint installation remains outside Git by policy; checker and this record preserve the durable rule |

## Review trace

按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）及项目
`skills/speckit-code-design/references/review-agent.md` 执行只读审查。基线为
`fbb4bcfe`；差异范围为 `skills/README.md`、`verify-spec-kit-sync.py` 以及 11 个本机
入口副本。审查覆盖每个入口的实际命令文本、checker marker 组、例外入口、返回码和
feature/product 证据边界；未发现 P1/P2/P3。

## Batch result

- `Static findings`: 初始缺口是共享 code-design preflight 没有显式出现在实际安装入口；已
  为 11 个入口补充相同的 `Feature Sync Preflight`，并在 checker 增加命令和
  `--require-entrypoints` marker 强制，复审无 actionable finding。
- `Compile/build misses`: `none`; checker-only Python syntax check passed。
- `Runtime/test misses`: 强制 checker 正常路径通过；删除 marker 的临时入口按预期失败；未
  执行 native request/result、maintained caller、跨进程 transport 或 T016 qualification。
- `Build measurement`: `BUILD_NOT_APPLICABLE`; no native target/source closure。
- `Behavior result`: `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS` for checker enforcement;
  `CLOSED_FOR_VALIDATION` for this workflow boundary; not product qualification。
- `Evidence / remaining`: 产品父任务和 T016 仍为 `PARTIAL`/`UNQUALIFIED`，必须继续沿
  P1–P7 真实生产出口推进。

## Batch growth decision

共同入口规则、checker marker、同步 README 和本机入口副本共享同一 workflow 出口，故保留
在一批。稳定出口出现后立即停止扩张；产品实现、native build、caller migration 和资格
验证不纳入本批，后续规则变化使用新的 Batch ID。

## Closure decision

`CLOSED_FOR_VALIDATION` 仅适用于 Spec Kit 入口级 preflight enforcement。它不改变 Spec182
任何产品任务状态，也不能将技能同步或入口检查写成 native behavior、parity 或
qualification PASS。

## Batch Retrospective

- `static`: 原缺口是共享 skill 有规则但安装入口未显式执行；新增入口文本和 checker marker
  后复审通过。
- `compile/link`: `none`; no native source or target changed。
- `runtime/test`: 正常路径和故意缺 marker 的失败路径均按约定返回；产品运行仍未观察。
- `unobserved`: 其他机器的安装副本及未来 feature 的实际执行仍需各自运行强制预检。

`Changed gate`: checker 的 `ENTRYPOINT_MARKER_GROUPS` 新增 `verify-spec-kit-sync.py`
和 `--require-entrypoints` 两项；这改变了同步检查本身，并覆盖此前“入口文件存在但未
执行预检”的首个 workflow 漏检边界。
