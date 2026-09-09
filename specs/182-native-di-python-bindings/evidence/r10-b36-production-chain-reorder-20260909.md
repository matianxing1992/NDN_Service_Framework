# R10-B36 Remaining Production Chain Reorder

**Date**: 2026-09-09
**Baseline**: `3620e35b`
**Scope**: execution-order documentation only; no product source, task acceptance gate, or parent status changed.

## Decision

在暂停新增实现期间，将现有 T010–T017 execution cards 按真实生产出口排列为 P1–P7：

1. P1：Native requester→Core/Provider 的 unary/stream 结果边界（`T004-A`, `T008-A/B`, `T010-A/B`）。
2. P2：Provider worker/跨进程首轮与续接，包括 receipt/control/commit、恢复和清理（`T010-C`, `T011-C`）。
3. P3：维护中的 YOLO caller 通过 native facade/`REPO_REF` 完成真实请求与回滚证据（`T012-A/B`, `T013-A`, `T013-F`）。
4. P4：Qwen/streaming observer 与 native conversation owner 的真实两轮/replacement（`T013-C/D/E`, `T011-C`）。
5. P5：在 P3/P4 均有真实结果后，退役旧 runtime/default import graph（`T013-B`）。
6. P6：隔离 collector/harness 及 no-Python 反例（`T014-A/B`）。
7. P7：跨任务收敛、完整资格和 handoff（`T015-A` → `T016-A` → `T017-A`）。

P1–P4 各自拥有不同 caller、进程边界、状态机或 selector，稳定出口后必须拆新 Batch ID；
P5–P7 依赖前置真实结果。现有局部 fixture、CLI smoke 和 Python compatibility PASS 不会
被该表提升为 native qualification。缺少 NFD/node context 的 T016 仍保持 `UNQUALIFIED`。

## Static review trace

使用官方只读 [review-agent](https://github.com/openai/codex/blob/main/codex-rs/skills/src/assets/samples/review-agent/SKILL.md)
（本机 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）。审查基线为
`3620e35b`，完整变更范围是 `plan.md`、`tasks.md` 和本 evidence；同时读取现有 R5-B3
caller matrix、R10-B31/B33 native request 边界和 T016 preflight evidence。审查无 P1/P2/P3
回归；重排只新增可执行顺序说明。

## Minimum Review Record

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | T010–T017 execution cards; R5-B3 caller matrix; R10-B31/B33 request boundaries | `rg -n "T010|T011|T012|T013|T014|T015|T016|T017|R5-B3|R10-B3" specs/182-native-di-python-bindings/{plan,tasks}.md` | caller、Provider worker、Qwen/YOLO 和 retirement 的出口被分开；无代码回归 |
| `implementation and wire` | `covered` | existing card contracts and named native requester/provider/conversation boundaries | card/contract link inspection; no implementation edits | 每个阶段保留既有状态/契约 owner；缺 owner metadata 时继续 fail-closed |
| `test/harness/oracle` | `covered` | existing C++ selectors in R10-B31/B33/R4-B6; T014 harness; T016 matrix | selector/evidence link scan; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | 真实 selector 与 external owner 依赖已映射；本批不运行新测试 |
| `build/source closure` | `N/A` | no product source or target changed | changed-path review; no build command | 不适用；本批不声称 `BUILD_PASS` |
| `migration/evidence` | `covered` | `plan.md`, `tasks.md`, R5-B3, R10-B31/B33, T016 preflight and this record | link/heading scan; `git diff --check` | 首个失败边界和 `UNQUALIFIED` 外部环境状态保留；无 evidence 覆盖升级 |

## Batch result

- **Review trace**: official `review-agent` path/SHA and baseline/diff scope are recorded above;
  all five lane queries were rerun after the plan/tasks edits with no actionable finding.
- **Closure decision**: `CLOSED_FOR_VALIDATION` for the execution-order documentation boundary;
  P1 is the next implementation gate and P2–P7 remain dependent. No product acceptance is closed.
- **Static findings**: none; the previous ambiguity was execution-order ambiguity, not a product defect.
- **Compile/build misses**: none observed; documentation-only.
- **Runtime/test misses**: not observed by this batch; all P1–P7 behavior gates remain owned by
  their existing cards and are not claimed complete.
- **Build measurement**: `BUILD_NOT_APPLICABLE`; only text/link/validator checks ran, so no speedup
  conclusion is drawn.
- **Behavior result**: `STATIC_PASS` and `CLOSED_FOR_VALIDATION` for the planning boundary only;
  not `QUALIFICATION_PASS`.
- **Evidence / remaining**: next implementation work starts at P1 and stops at its first stable
  native result. Product parents remain `PARTIAL`/`UNQUALIFIED` in [`tasks.md`](../tasks.md).

### Batch growth decision

The batch has one stable exit: a caller-shaped P1–P7 order that maps every remaining card to a
distinct production result. No caller, state machine, selector, source closure, or hard acceptance
dependency was added. Any such change starts a new Batch ID.

### Batch Retrospective

- `static`: no new miss; caller/process/qualification boundaries are now explicit.
- `compile/link`: not observed; no product target changed.
- `runtime/test`: not observed; existing focused and external gates remain pending.
- `unobserved`: real Provider worker/cross-process execution, maintained caller/no-Python, and T016
  remain open; this planning record does not promote them.
