# R10-B43 Spec Kit Entrypoint Synchronization Check

日期：2026-09-09
状态：`DONE` for the shared workflow boundary; product tasks remain unchanged.

## Scope and stable exit

本批把共享 Spec Kit 规则的副本核对从人工 `rg` 清单变成可重复检查。检查器位于
`skills/speckit-code-design/scripts/verify-spec-kit-sync.py`，覆盖三份 Spec Kit 模板、
11 个生成/澄清/计划/任务/分析/审计/执行入口，以及 `CODEX_HOME` 下的共享
`speckit-code-design` 文件。只负责更新 agent context 指针的
`speckit-agent-context-update` 不生成或验收 feature artifact，因此明确排除。

稳定出口是强制模式返回 `PASS`，且实际入口和个人共享文件均与版本化规则一致。这个
出口只证明工作流同步，不证明任何 native 行为、构建、绑定、跨进程传输或资格验收。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `.agents/skills/speckit-{specify,clarify,plan,tasks,analyze,audit,implement,converge,checklist,constitution,taskstoissues}/SKILL.md` | `verify-spec-kit-sync.py --require-entrypoints --require-personal` | 11/11 entries expose the shared contract; agent-context-only entry is intentionally excluded |
| `implementation and wire` | `covered` | `skills/speckit-code-design/references/batch-quality-gates.md`, `skills/README.md`, `verify-spec-kit-sync.py` | `rg -n 'Entrypoint Synchronization|Command Output Contract|Coverage matrix' ...` | shared reference and checker agree on the same entrypoint set and warning/failure semantics |
| `test/harness/oracle` | `covered` | checker marker groups and temporary stale-copy/missing-install probes | normal run; stale personal copy returns exit 1; absent optional installs return exit 0 with warnings | stale content is rejected; optional absence is visible without making a fresh clone fail |
| `build/source closure` | `N/A` | documentation/checker only; no product target or generated native source | `python3 -m py_compile skills/speckit-code-design/scripts/verify-spec-kit-sync.py` | no product build; Python syntax and source file closure pass |
| `migration/evidence` | `covered` | personal `~/.codex/skills/speckit-code-design` copy and this evidence record | SHA-256 comparison for all 11 shared files; `git diff --check`; validator | personal copy matches versioned files; this record preserves the synchronization result and limits |

## Review and validation

官方只读 `review-agent`：`/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`；基线
`327779cd97e1e26881def032a517ca1396ec22a3`；完整差异范围为本批 checker、README、
shared reference、plan/tasks 和本 evidence。审查实际核对了入口集合、模板职责、个人
副本比较、缺失/过期分支、返回码和文档链接；首轮发现 source 文件缺失会抛异常，已改为
显式 `versioned shared file missing` finding，复审无 P1/P2/P3 finding。

实际命令与结果：

```text
python3 -m py_compile skills/speckit-code-design/scripts/verify-spec-kit-sync.py                  PASS
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal
PASS: 11/11 local entrypoints; personal shared skill=present
temporary stale personal copy probe: exit 1 (expected)
temporary absent-install probe: exit 0 with warnings (expected)
git diff --check                                                                                 PASS
python3 specs/182-native-di-python-bindings/checklists/validate_design.py                        ok=true
```

## Batch result

- `Static findings`: 首轮 checker 自身异常边界已修复；复审覆盖入口、模板、共享 reference、个人副本和返回码，无 actionable finding。
- `Compile/build misses`: `none`; documentation-only，未修改 native target。
- `Runtime/test misses`: `none observed` for the checker; native request/result、maintained caller/no-Python、cross-process and T016 remain unobserved here。
- `Build measurement`: `BUILD_NOT_APPLICABLE`; Python syntax check only，no `-j` build。
- `Behavior result`: `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `FOCUSED_BEHAVIOR_PASS` for the checker; `CLOSED_FOR_VALIDATION` for synchronization only。
- `Batch growth decision`: 稳定出口是强制同步检查通过；不再吸收产品实现、测试迁移或资格任务，后续规则变化继续以新的文档批次处理。
- `Closure decision`: `CLOSED_FOR_VALIDATION` for R10-B43；Spec182 product parents and T016 remain `PARTIAL`/open。

## Batch Retrospective

- `static`: 首轮发现 source-missing 异常未被归类；新增显式 source-file 检查后复审通过。
- `compile/link`: `none`; no native source or target changed。
- `runtime/test`: checker 的 stale/absent installation probes match the contract; no product runtime was run。
- `unobserved`: no native behavior, caller migration, cross-process transport, or qualification evidence is claimed。

`Changed gate`: 增加可执行的入口 marker groups、模板职责 marker、个人副本 SHA 比较和
缺失/过期返回码探针；这覆盖了此前只靠人工 `rg`/SHA 清单、容易漏掉入口副本的同步边界。
