# Spec184 Native MiniNDN Post-ACK Caller Routing

**Status**: `CLOSED_FOR_VALIDATION` for the caller route; Spec184 qualification remains `PARTIAL`
**Date**: 2026-09-12
**Scope**: `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` → maintained Qwen User
**Source checkpoint**: `7488ac08`

本批修正 MiniNDN launcher 与 native requester 的接线，不改变 C++ 策略或 Provider 状态机。
源码核对确认 `NativeInferenceClient` 在 `ACK_CLOSED` 回调中调用
`planNativeRequest`；该函数随后执行 native splitter、placement、artifact preparation 和
sealed Selection。`DI_NativeProviderExecutable` 启动时只保留 metadata-only runner slot，
认证 Selection 后由 `runnerPreparationFactory` 调用 canonical ONNX assembly。因此模型角色的
正式组装边界仍是 C++ Provider 的 post-Selection 阶段。

## Defect and repair

修复前，runner 在 `runtime=qwen-onnx-cpu-native` 下总是向 User 传递
`--native-cpu-provider`，随后又追加 `--native-requester-config`。`user.py` 的循环先判断
`native_cpu_provider`，所以会进入旧的逐 token tensor-bundle diagnostic 分支，配置化的
`APPClient.request_native_reference` 分支不会执行。这会把“native requester”命令误接成兼容
诊断路径。

现在 `build_native_user_args` 明确选择两条互斥路线：

- 有 `--native-requester-config`：只传配置，User 进入 `request_native_reference`，其 ACK、
  native planning 和 Provider post-Selection assembly 由 C++ owner 负责。
- 没有配置：保留 `--native-cpu-provider` 与 Qwen service manifest，作为显式兼容诊断路径，
  不计入 native requester qualification。

User 入口另外拒绝两种标志同时出现，并把 native final payload 的协议字段 `tokenIds` 映射到
现有首 token oracle；空或错误 schema 直接失败。

## Five-lane gate

| Lane | Result | Evidence |
| --- | --- | --- |
| production entry / callers | `PASS` | helper is called at the MiniNDN User command construction site; native and compatibility branches are mutually exclusive |
| implementation / wire | `PASS` | `NativeInferenceClient::ackClosed` → `planNativeRequest`; Provider metadata-only startup → post-Selection `runnerPreparationFactory` |
| test / harness / oracle | `PASS` for focused route | `test_spec180_qwen_entrypoint.py` asserts both native and compatibility argv; `test_spec182_legacy_exclusion.py` remains green |
| build / source closure | `BUILD_NOT_APPLICABLE` | Python launcher/User-only change; `py_compile` passed and no C++/ABI input changed |
| migration / evidence | `PARTIAL` | route is corrected, but real native Qwen model execution, no-Python process qualification and Python retirement remain open |

## Validation

- Read-only static review followed the installed `review-agent` procedure over the complete diff;
  no actionable regression was found.
- `git diff --check`, AST parsing, deterministic route assertions and `py_compile` passed.
- Qwen launcher and legacy exclusion suites: **14/14 passed**.
- Native backend registration plus native binding suites: **27/27 passed** when bound to the existing
  candidate `build-spec184-b5-candidate-r4/examples/di-native-provider`.
- The active Spec structural audit passed (`functional_requirements=6`, `success_criteria=5`,
  `tasks=8`, `tasks_complete=6`, traceability present); the shared Spec Kit synchronization check
  passed (`11/11` entrypoints) and the design validator passed with `errors=[]`.
- The first unbound invocation failed before product execution because the default
  `build-system-j2/examples/di-native-provider` path is absent. The raw first-boundary log is
  `.codex-tmp/spec184-native-route-20260912/default-path.log` (SHA-256
  `62f15a4d8f280a337dfd54b7b11627fa38997349b02221c9ae6a24624257b941`); the corrected candidate-path
  run is `.codex-tmp/spec184-native-route-20260912/candidate-path.log` (SHA-256
  `6836801e4ba6dca51e443d5bf2ddfa57d8354ff9eaa4925ba9657a4661edb1fa`).

## Closure decision and limits

`CLOSED_FOR_VALIDATION` applies only to this launcher route. No MiniNDN run was started in this
batch, so there is no new protocol, numeric, model, no-Python, or qualification result. Spec184
T007 stays `IN_PROGRESS`/`PARTIAL`; A3 remains `WAITING_EXTERNAL_INPUT` for Qwen3.6-27B and A4
retirement/negative rows remain open. The next executable step is a real native-config Qwen request
on an input set whose canonical source and tokenizer identity are available, followed by current
candidate evidence refresh.
