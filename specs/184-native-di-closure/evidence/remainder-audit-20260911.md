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

## Superseding execution record (2026-09-11)

上述旧表保留了 A0/A1/A2 尚未执行时的首边界。以下记录是同一工作单元完成后的当前
状态覆盖，替代旧表中的 `OPEN`/`NOT_RUN`，不改变外部模型和继承行的边界。

| Gate | Current status | Evidence and result |
| --- | --- | --- |
| T007-A0 | `PASS_FOR_ROW` | `build-spec184-b5-candidate-r4/spec180-native-build.json`，SHA-256 `f83d4499fc7271bdc205fb56d212e9688741b754e1b964ce8ecb3ccb2c0e99b1`；`spec180_native_build.py verify` exit `0`，system `/usr/bin/g++ -B/usr/bin`，`-j4`，candidate-first paths，`binding_reused=true`。 |
| T007-A1 | `PASS_FOR_ROW` | Root MiniNDN Y-A run `r51`：`.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/`，terminal response、10 lifecycle events、child exits and no cleanup error；C++ numerical oracle `matched=true`，shape `[1,50,6]`，`maxAbsError=0.0005340576171875`。 |
| T007-A2 / Y-B | `PASS_FOR_ROW` | Root MiniNDN Y-B run `r37`：`.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/`，terminal `YOLO_ACK_DRIVEN_RESULT status=true`，10 lifecycle events，四 Provider child cleanup；C++ ORT CPU runtime evidence records `realCompute=true` for Backbone/Detect shards and native postprocess for Merge。 |
| T007-A2 / Y-N | `PASS_FOR_ROW` | Root MiniNDN Y-N run `r50`：`.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/y-n-matrix-result.json`，SHA-256 `009e07d68a766f1545312b87261884e789cbb207db4404432f1ac3729161215a`；`Y-N-O/C/P/R/I/E/L` all `PASS`，E independently covers `EXPIRED`、`FORGED_AUTHORITY`、`WRONG_RECIPIENT`，cleanup errors `0`。 |

Y-A 与 Y-B 的 `yolo-numerical.json` 都记录相同的 bounded YOLO26n fixture（shape
`[1,50,6]`、`maxAbsError=0.0005340576171875`、`atol=0.001`、`rtol=0.0001`），并通过
当前候选的 native Provider；Python 只启动外部设施和解析证据。Y-N 重跑前修复了收集器对
ndn-cxx 时间戳日志前缀的归一化，修复后的 harness 经 `py_compile` 与 `git diff --check`。

本机的模型边界保持不变：只能运行 `Qwen3-0.6B` smoke/ABI fixture，无法运行契约要求的
`Qwen/Qwen3.6-27B`；A3 仍为 `WAITING_EXTERNAL_INPUT`。A4 的继承 negative/retirement、
I05 `UNQUALIFIED`、Python retirement 以及 SIF/Tiger external owner 仍未闭合，所以这次
只提升 A0–A2 行状态，T007 总体继续 `IN_PROGRESS`/`PARTIAL`，T008 继续阻塞。

## Environment capability checkpoint (2026-09-12)

The current host has six logical CPUs and 11 GiB RAM. It can run the local
`Qwen3-0.6B` smoke/ABI fixture but cannot execute the contract-required
`Qwen/Qwen3.6-27B`; the exact model family, manifest, tokenizer, CUDA runtime
and staged model objects remain external-owner inputs. The detailed boundary is
recorded in [T007 model capability evidence](t007-model-capability-20260912.md).

An attempt to add the C++ `integration-tests` selector to the current r4
candidate stopped before compilation because that candidate was configured with
`--with-examples` and has no `integration-tests` task generator. Raw output is
`.codex-tmp/spec184-qwen-smoke-20260912/waf-build.log` (SHA-256
`1f49b7cd938da12e8169c4248501b832b85b8fcdb66b2fc1352092dd413ece62`). This is
a Waf configuration boundary, not a model or protocol result; the candidate was
not reconfigured solely to manufacture a selector binary.

A second bounded attempt used the older candidate's `integration-tests`
executable with r4 libraries first in `LD_LIBRARY_PATH`. It emitted the native
business marker and then SIGSEGVed at `0x00000080` (exit `201`). This mixed
binary/library combination is an ABI boundary and is rejected as evidence; its
raw log is `.codex-tmp/spec184-qwen-smoke-20260912/integration-qwen.log` with
SHA-256 `33c155fb5124cd7249551586e6b5ee1b5658334503d05ac09d4ec8f4d0a85cde`.
The test executable and native libraries must be rebuilt from one configured
tree before any local 0.6B smoke result can be accepted.

The disposition is unchanged: A3 stays `WAITING_EXTERNAL_INPUT`, A4 remains
the only local remainder, and T008 remains `BLOCKED_BY_T007`. No 0.6B smoke
result is used as 27B qualification evidence.

## Superseding candidate refresh (2026-09-12)

The registration-lifetime and Provider-scoped request changes were reviewed and
rebuilt from the same configured r4 tree. The weak retry-owner guard and empty
buffer guard were added during that review. The refreshed receipt verifies with
exit `0` and SHA-256
`219780a01753551d801ac6190ac334799ee948c5fbb7662e73041d40936f5e68`;
the detailed review/build record is [current candidate refresh](a4-current-candidate-refresh-20260912.md).

Because the framework and DI library hashes changed, the former Y-A `r51`,
Y-B `r37`, and Y-N `r50` results are retained as historical evidence but are
not current-candidate qualification. The ordered remainder is now:

| Gate | Current status | Required next evidence |
| --- | --- | --- |
| T007-A0 | `PASS_FOR_ROW` | refreshed receipt and loader identity, already verified in the current refresh record |
| T007-A1 | `NOT_RUN_CURRENT_CANDIDATE` | root Y-A C++ numerical oracle, terminal child exits and cleanup using the refreshed candidate |
| T007-A2 | `NOT_RUN_CURRENT_CANDIDATE` | root Y-B and Y-N protected/negative matrix using the same refreshed candidate |
| T007-A3 | `WAITING_EXTERNAL_INPUT` | exact `Qwen/Qwen3.6-27B` bundle and external result; local 0.6B remains smoke-only |
| T007-A4 | `PARTIAL` | inherited negative/retirement rows with complete current identity; I05 may remain explicit `UNQUALIFIED` |
| T008 | `BLOCKED_BY_T007` | final handoff only after T007 overall qualification passes |

No current-candidate YOLO run has been claimed yet. The next local action is
Y-A, followed by Y-B/Y-N if its terminal cleanup is complete; these runs do not
require or imply execution of the unavailable 27B model.

## Current candidate Y-B/Y-N closure (2026-09-12)

The refreshed r4 candidate now has current-candidate `PASS_FOR_ROW` results for Y-A, Y-B and
Y-N. Y-B is recorded at `.codex-tmp/spec184-yolo-Y-B-output-20260912-r59/` with launcher
SHA-256 `746f2df2469901050676abe077154a0cfe6c7354db0a354d43c431ddc8639da5`. Y-N completed
at `.codex-tmp/spec184-yolo-Y-N-output-20260912-r60/`; the launcher SHA-256 is
`86cbd5a303b6cd5cf5db51ec5aa4568ec092aa18d09a4e762d32a306b7d694f1`, and the seven-subcase
matrix SHA-256 is `009e07d68a766f1545312b87261884e789cbb207db4404432f1ac3729161215a`.
The Y-N aggregate is `PASS`; `Y-N-O/C/P/R/I/E/L` all passed at their declared boundaries,
including native rejection evidence for the E permutations and controlled cleanup.

The ordered remainder is therefore:

| Gate | Current status | Boundary |
| --- | --- | --- |
| T007-A0 | `PASS_FOR_ROW` | refreshed receipt and loader identity |
| T007-A1 | `PASS_FOR_ROW` | current-candidate Y-A C++ numerical oracle and cleanup |
| T007-A2 | `PASS_FOR_ROW` | current-candidate Y-B protected path and Y-N negative matrix |
| T007-A3 | `WAITING_EXTERNAL_INPUT` | exact `Qwen/Qwen3.6-27B` on experiment owner; local 0.6B is smoke-only |
| T007-A4 | `PARTIAL` | inherited negative/retirement rows, I05 observation boundary, and Python retirement |
| T008 | `BLOCKED_BY_T007` | final handoff requires T007 overall qualification |

This is a local YOLO closure update only. It does not relabel the unavailable 27B model, promote
I05 `UNQUALIFIED`, or claim Spec184 completion.

## Current binding and Y-A refresh (2026-09-12)

The first current-candidate Y-A attempts stopped at `CASE_RUNTIME_PROCESS_START_FAILED:control`;
the Controller log contained only construction output because the Python extension had not been
rebuilt after the framework/DI refresh. The raw launcher logs are
`.codex-tmp/spec184-yolo-Y-A-run-20260912-r52a.log`, `r53.log`, `r54.log`, and `r55.log`.
These are startup/identity boundaries, not protocol or model failures.

The binding was then rebuilt against the candidate NAC-ABE, NDN-SVS and DI libraries. The build
log is `.codex-tmp/spec184-python-binding-rebuild-20260912.log` (SHA-256
`bd74baa072ab553711bd2a5e03626ee18e704ee459594868ddc4e0f2675b5d02`); the refreshed receipt
build and verify logs are `.codex-tmp/spec184-native-receipt-after-binding-20260912.log` and
`.codex-tmp/spec184-native-receipt-verify-after-binding-20260912.log`, with verify exit `0`.
The current extension hash is
`9e41958e73cb5b805e4d8261a901da9c43aea0ada85bfcf472808d7709ff2d2b` and the receipt hash is
`2fbb8f40b2c51e1d3c0334387759112f90112264a2983157abe9421202b02b15`.

With that identity, root MiniNDN Y-A passed. The output directory is
`.codex-tmp/spec184-yolo-Y-A-output-20260912-r58/`; the launcher log is
`.codex-tmp/spec184-yolo-Y-A-run-20260912-r58.log` (SHA-256
`bc2320fea3a85682eea468cb2d59599b76bda6c57474a647fb1a7a9ba4b867cb`). The terminal result is
`YOLO_ACK_DRIVEN_RESULT status=true`; the C++ numerical oracle matched with shape `[1,50,6]`
and `maxAbsError=0.0005340576171875`. Child exits and cleanup are complete.

| Gate | Current status | Next action |
| --- | --- | --- |
| T007-A0 | `PASS_FOR_ROW` | receipt now includes the rebuilt binding identity |
| T007-A1 | `PASS_FOR_ROW` | retain r58 evidence and proceed to Y-B |
| T007-A2 | `NOT_RUN_CURRENT_CANDIDATE` | run Y-B then Y-N against the same r4 candidate |
| T007-A3 | `WAITING_EXTERNAL_INPUT` | exact Qwen3.6-27B remains external; 0.6B is smoke-only |
| T007-A4 | `PARTIAL` | continue inherited negative/retirement closure |

The subsequent protected multi-provider Y-B run also passed on the same binding-refresh
candidate. Output is `.codex-tmp/spec184-yolo-Y-B-output-20260912-r59/`; launcher log is
`.codex-tmp/spec184-yolo-Y-B-run-20260912-r59.log` (SHA-256
`746f2df2469901050676abe077154a0cfe6c7354db0a354d43c431ddc8639da5`). Its C++ numerical
oracle matched, all seven child processes exited and cleanup completed. A2 remains `PARTIAL`
until the Y-N negative matrix is rerun.

## External cache correction (2026-09-12)

The earlier inventory language saying that only the 0.6B payload was present is historical and
is superseded for current external-state description. A later read-only scan found an incomplete
`Qwen/Qwen3.6-27B` Hugging Face cache: one of 15 expected shards is present, 14 are missing,
`refs/main` is absent, and no Spec184 candidate directory exists. The exact counts and path are
recorded in [A4 external boundary](a4-retirement-external-boundary-20260912.md). This changes
the description of the external cache, not the qualification status: A3 remains
`WAITING_EXTERNAL_INPUT`, A4 remains `PARTIAL`, and T008 remains `BLOCKED_BY_T007`.
