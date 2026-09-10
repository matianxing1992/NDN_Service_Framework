# R11-B8-G4 Native Checkpoint Handle Export

## Status

`CLOSED_FOR_VALIDATION` for this bounded handle/export boundary. The parent
R11-B8 Maintained Callers task remains `PARTIAL`; this batch does not claim
APPEND_DELTA caller migration, cross-process Provider KV recovery, legacy zero-use,
no-Python closure, or T016 qualification.

## Scope and implementation

本批次把 conversation commit 的 native owner 与公开结果句柄接通。`NativeInferenceHandle`
在 `NativeConversationCoordinator::commitTurn` 成功返回后保存原样 authenticated opaque
checkpoint wire；它不复制 transcript、KV 或 Provider state。句柄只有在终态
`NativeRequestStatus::Succeeded` 时导出 checkpoint，ordinary、in-flight、failed 和
cancelled request 都 fail-closed。pybind 和 SDK facade 只把该 wire 转为 `bytes`，不在
Python 重建会话状态。

Changed files:

- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp`
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`
- `pythonWrapper/src/ndnsf/di_bindings.cpp`
- `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`
- `tests/unit-tests/di-native-client.t.cpp`
- `tests/integration-tests/ndnsf-di-core-flow.t.cpp`

## Review trace

按官方只读技能 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的 defect-first 方法检查
完整 diff、所有调用点、锁和终态发布顺序、所有权、ABI、构建注册及新增断言。审查确认：

- checkpoint 只来自 `commitTurn` 返回值，并在同一 operation mutex 下发布；
- `markTerminal(Succeeded)` 的发布顺序不会让未提交 turn 暴露 checkpoint；
- empty/failed/cancelled/in-flight handle 不泄露 checkpoint；
- integration assertion 将 handle wire 与 coordinator record wire 逐字节比较；
- Python 层没有引入 planner、transcript 或 Provider-state fallback。

`git diff --check` and Python `py_compile` passed. No actionable static finding remained.

## Validation

Build boundary used the system-first compiler and the configured candidate Waf output
`.codex-tmp/spec182-r11-b2-fresh-20260910/build` with `-j2`:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin ./waf build --targets=unit-tests,integration-tests -j2 -v
result: exit 0; current NativeInferenceClient.cpp compiled and both test binaries linked
PATH=/usr/bin:/bin:/usr/sbin:/sbin ./waf build --targets=ndnsf-distributed-inference -j2 -v
result: exit 0; current NativeInferenceClient.cpp linked into libndnsf-distributed-inference.so
```

C++ primary behavior:

```text
unit-tests --run_test=Spec182NativeInferenceClient
result: 2 cases, no errors
unit-tests --run_test=Spec182ClientState/CoreIoRejectsBlockingResultButAllowsPollAndCancel
result: 1 case, no errors
integration-tests --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation
result: real Provider conversation, no errors, SPEC182_NATIVE_DI_REQUEST_RESULT_OK
unit-tests --run_test='Spec182*'
result: 257 test cases, no errors
```

Python secondary binding checks used the matching candidate Core/DI, NAC-ABE and SVS paths:

```text
pythonWrapper/setup.py build_ext --inplace --force --parallel 4
result: exit 0
PYTHONPATH=pythonWrapper python3 -c 'import ndnsf; ...'
result: import PASS; NativeInferenceHandle.conversation_checkpoint is exported
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_native_closure.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
result: 81 passed in 3.03s
```

`ldd` resolves `libndnsf-distributed-inference.so` and
`libndn-service-framework.so` from the same candidate Waf directory,
`libnac-abe.so` from `/home/tianxing/NDN/nac-abe-integration-182/install/lib`, and
`libndn-svs.so` from `/home/tianxing/NDN/ndn-svs/build`.

## Miss taxonomy and boundary

The first extension import after its source rebuild failed with an undefined
`conversationCheckpoint` symbol because the shared DI library had not yet been relinked.
The raw command output is preserved in
`.codex-tmp/spec182-r14-checkpoint-20260910/python-focused.log` and the corrected relink/build
logs. Relinking the current library and rebuilding the extension removed the boundary; no
runtime test failure remained.

This batch proves native checkpoint publication and thin binding forwarding. It does not prove
that a maintained Qwen/streaming caller can construct an APPEND_DELTA continuation, that a
Provider retains KV after restart, or that all 16 caller groups and final qualification gates
are complete. The next stable batch must consume this handle export in one native APPEND_DELTA
caller before expanding to the remaining maintained callers.
