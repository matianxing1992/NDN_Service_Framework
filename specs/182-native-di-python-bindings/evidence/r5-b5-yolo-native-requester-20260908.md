# R5-B5 Maintained YOLO Native Requester

## Status

`STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL`。

本批把维护中的 `DI_NativeRequester` 组合入口接到 shared
`nativeRequestRuntimeFromJson`。CLI 仍负责读取 operator 配置、catalog 源、grant key 和
offer admission；runtime 的 contract、身份、摘要、保护要求、budget、state mapping 与
progress limits 统一由 native parser 校验。没有新增 Python planner 或 fallback。

## Static review

按 `$review-agent` 的只读 defect-first 方法检查完整 diff、调用方和配置契约：

- CLI 不再直接写入 `NativeRequestRuntime` 字段；只生成 parser 的 v1 JSON 并绑定已加载的
  catalog/grant owner。
- `state_mapping` 从 native catalog 直接序列化，避免重新推导 role/state 名称。
- parser 返回的 catalog/grants 被同一个 `NativeInferenceClient` 使用，配置身份不能在
  parser 之后被替换。
- Python 检查只验证 binding/source 接线，不承担 DI 行为判定。

静态复核未发现可执行的新缺陷。真实 Core/Provider request、维护中的 Python YOLO user
切换、Qwen/streaming caller、旧 runtime retirement 和 T016 仍未完成。

## Verification

All commands ran from the repository root with the system-first toolchain:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf build --out=build-nac182 --targets=DI_NativeRequester,unit-tests -j4
```

Waf entered `.codex-tmp/spec182-r4-b2/build`, compiled and linked
`examples/DI_NativeRequester` successfully in 14.173 seconds. `vmstat 1` showed no sustained
swap-in/out after its first line; the host still reports existing swap allocation, so this is a
build observation rather than a memory qualification.

```text
.codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester --help     -> 0
.codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester            -> 2 (usage)
.codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester \
  --config invalid-schema.json --input missing --output unused        -> 1
  stderr: NATIVE_REQUESTER_FAILED: unsupported requester configuration

.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test=Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift
  -> 1 case, no errors

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
  -> 25 passed
```

The parser selector covers the deterministic YOLO catalog/grant fixture and rejects unknown
fields, adapter/requester/epoch drift, disabled protection, zero budget, malformed types and
state mapping drift. The CLI selectors prove only local entry/configuration behavior; they do not
prove a network request or Provider execution.

## Remaining boundary

`DI_NativeRequester` now has one native runtime composition path. The maintained
`examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` still uses the existing Python
ACK-driven coordinator until a later caller migration card supplies its operator catalog/grant
configuration. No MiniNDN, SIF, Tiger or cross-process qualification was run in this batch.
