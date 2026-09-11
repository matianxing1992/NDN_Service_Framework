# Spec184 Remaining Qualification Audit

**Date**: 2026-09-11
**Status**: `PARTIAL` / audit and execution reordering only; no final qualification claim
**Candidate**: current `FRESH_LOCAL_PARTIAL` record in [promotion candidate](../contracts/promotion-candidate.md); its ordered member-map digest is recomputed whenever this audit or another bound member changes.

本轮复核了当前 `spec.md`、`plan.md`、`tasks.md`、qualification matrix、promotion candidate、
最新失败记录、T007 process evidence，以及 YOLO/Qwen 入口和 C++ owner 调用链。已完成的
T001–T006 不重做；本记录只把剩余出口按可执行条件重新排序。

## Verified findings

| Area | Current result | Meaning for T007 |
| --- | --- | --- |
| C++ candidate closure | Full unit and integration exit `0`; unary/stream/continuation/recovery/replacement/grant process cases, I02–I08 bounded samples and counterexamples, no-Python ELF inspection, and root `PO-001-stream` are recorded in [T007 process qualification](t007-process-qualification-20260911.md) | These are bounded `PASS_FOR_ROW`/sample results. They do not close every inherited qualification row. |
| YOLO26n input preflight | Current runner `validate_inputs("Y-A")` passed before MiniNDN startup with the canonical package, checked-in registry, offer trust set, topology, config and current candidate library path. Log: `.codex-tmp/spec184-yolo-preflight-20260911-r7.log`, SHA-256 `93b82d3f922eee8864dd0a620bfd449ee79b62cf8a2b602337f17104930f7b15` | The real YOLO entry has usable inputs. The full owner run is still `NOT_RUN` because the local build guard requires a current `spec180-native-build.json` receipt, which is absent from `build-spec184-b5-candidate`. |
| YOLO local build guard | `python3 scripts/spec180_native_build.py verify --build-dir build-spec184-b5-candidate` returned `SPEC180_NATIVE_IDENTITY_REJECTED` because `build-spec184-b5-candidate/spec180-native-build.json` is missing. Log: `.codex-tmp/spec184-yolo-native-build-guard-20260911-r1.log`, SHA-256 `7e7bf009ebd960c9f4c132ae6ed511b56b2fb3f7d7bba71eee7fef928591011e` | This is a reproducibility/binding prerequisite, not a YOLO protocol failure. Generate a receipt for the current candidate or make the runner consume an explicit equivalent candidate receipt before starting MiniNDN. |
| Qwen3.6-27B | The local machine cannot execute the contract-required `Qwen/Qwen3.6-27B`. Only a cached `Qwen3-0.6B` payload is available; the Qwen entrypoint rejects a different model family/ID/revision and CPU fallback. | `0.6B` may be used for a C++ smoke/ABI check only. It cannot close the Qwen3.6-27B qualification row. The real Qwen row stays `WAITING_EXTERNAL_INPUT` and belongs to the experiment owner. |
| Isolation and collector | I02/I03/I04/I06/I08 have observed C++ policy `FAIL`, I07 has `PASS`, and I05 remains `UNQUALIFIED` at `TRACE_BUDGET_EXCEEDED` | These statuses are preserved; I05 requires a complete observation or remains unqualified. They are not reasons to rerun the same bounded samples blindly. |
| External boundary | SIF/Tiger ownership and external model execution remain separate from local qualification | T008 may record `TRANSFERRED` only after the local T007 result and bundle identity are complete. |

## Ordered remainder ledger

| Gate | Owner | Status | Exit condition | Next action |
| --- | --- | --- | --- | --- |
| T007-A0 current-candidate runtime receipt | local build owner | `OPEN` | Candidate-first requester/provider paths, compiler/dependency identity and a fresh receipt all bind `build-spec184-b5-candidate` | Produce or explicitly bind the receipt; do not use the historical `build-system-j2` default. |
| T007-A1 YOLO26n Y-A | local root MiniNDN owner | `NOT_RUN` | A0 passes; then C++ native numerical oracle, terminal result, child exits and cleanup are bound to the current candidate | Run one fresh Y-A case. |
| T007-A2 YOLO26n Y-B/Y-N | local root MiniNDN owner | `NOT_RUN` | A1 terminal path passes; protected and negative permutations reuse the same source/runtime identity | Run Y-B, then the declared Y-N subcases; classify input/startup failures separately. |
| T007-A3 Qwen3.6-27B | external experiment owner | `WAITING_EXTERNAL_INPUT` | Signed three-stage model manifest, tokenizer, CUDA runtime and model identity are present on the experiment machine | Transfer the exact candidate and execute there; never relabel 0.6B as Qwen3.6-27B. |
| T007-A4 inherited negative/retirement rows | local C++ owner plus matrix owner | `PARTIAL` | Each remaining row has a C++ selector or explicit external owner, complete observation, child exit and cleanup | Close only rows whose declared evidence is now complete; repair I05 collector evidence if possible. |
| T008 native handoff | local documentation owner | `BLOCKED_BY_T007` | T007 overall `QUALIFICATION_PASS`, final candidate map, and external rows marked `TRANSFERRED` | Do not start handoff or mark Spec184 complete before T007 passes. |

## Execution rule after this audit

继续执行时只按 `T007-A0 → T007-A1 → T007-A2 → T007-A4` 的顺序推进；`T007-A3` 在模型和
实验机可用后独立运行。每个 gate 仍遵循“逐小任务编码/静态审查 → 同批组合审查 → 统一
C++ 构建和测试”，但已经通过的 B1–B4、C++ 全套 sweep、I02–I08 bounded samples 和
Python harness 不再无条件重跑。Python 只能编排 MiniNDN 或收集证据，C++ 负责业务 oracle。
