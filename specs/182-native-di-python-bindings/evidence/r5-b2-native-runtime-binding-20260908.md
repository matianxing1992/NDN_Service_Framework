# R5-B2 Native Runtime Construction Binding

## Status and stable exit

`PARTIAL`。本批闭合了一个可观察的 binding/facade 出口：Python 可以把
`NativeRequestCatalog`、preparation catalog、offer admission、protected runtime
policy 和 authenticated grant owner 组合为 `NativeInferenceClient`，并通过公开
`InferenceClient.request_native()` 直接提交，不经过 Python planner。catalog source
校验、身份/密钥处理、grant publication、planning、authorization 和 wire 仍由 C++
owner 执行。

真实模型请求 parity、真实 Provider 负例、maintained caller 全量迁移、旧 runtime 退出、
跨进程交付和 T016 qualification 尚未完成，因此 T012-B 保持 `PARTIAL`，T013 及后续
任务不因本批通过而放行。

## Members and review

| Member | Scope | Result |
| --- | --- | --- |
| RB-1 | Export native catalog/runtime/preparation/admission/grant composition types from the single pybind entry | `STATIC_PASS`; `BUILD_PASS`; binding focused behavior passed |
| RB-2 | Bind an existing Core `ServiceUser` to native preparation, grant owner and configured requester | `STATIC_PASS`; `BUILD_PASS`; constructor validation and import checks passed |
| RB-3 | Expose explicit Python facade route and public `InferenceClient` export without planner fallback | `STATIC_PASS`; Python compatibility suite passed; real native request remains deferred |

Each member was checked with the read-only official review-agent profile at
`/home/tianxing/.codex/skills/review-agent/SKILL.md`. The review covered complete diffs,
production callers, test registration, source/link closure, ownership/lifetime, identity
binding and the no-fallback call path. One actionable issue was found and repaired: grant
configuration could name a requester different from the bound `ServiceUser`; the native
factory now rejects that mismatch before loading the remaining grant material.

## Coverage matrix

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry/callers | covered | `NativeServiceUser::nativePreparation`, `NativeServiceUser::nativeGrantClientFromConfig`, `NativeServiceUser::nativeInferenceClientConfigured`, `APPClient::request_native`, `InferenceClient::request_native`; queried with `codegraph explore "NativeServiceUser nativeGrantClientFromConfig nativeInferenceClientConfigured APPClient::configure_native_requester InferenceClient::request_native"` and `rg -n "native_(preparation|grant_client_from_config|inference_client_configured|request_native|configure_native_requester)"` |
| implementation and wire | covered | `pythonWrapper/src/ndnsf/di_bindings.cpp` exports native DTO/runtime owners; `pythonWrapper/src/ndnsf/_ndnsf.cpp` binds Core user, Ed25519 keys, grant issuer, and native requester; no new Python planner, subprocess, or wire implementation |
| test/harness/oracle | covered for binding/facade closure; gap for real request parity | `test_spec182_native_bindings.py` 24 cases; app SDK/legacy/closure compatibility selection 32 cases; invalid catalog schema, public export, identity guard and explicit no-fallback route are checked; real Provider/request oracle remains T012-B/T013/T016 |
| build/source closure | covered for local candidate | single `pythonWrapper/setup.py` extension target linked candidate Core/DI, NAC-ABE and SVS paths; corrected build exited 0; `vmstat 1` follow-up had no sustained `si`/`so`; source and RPATH boundary recorded in [T012-A ABI evidence](t012-a-binding-abi-20260908.md) |
| migration/evidence | gap | public facade route is additive and explicit; maintained caller migration, legacy retirement, cross-process packaging and qualification remain T013/T014/T016; raw focused outputs are retained under `.codex-tmp/spec182-r5-b2-native-runtime-binding-20260908/` |

## Miss taxonomy

- **Static findings**: requester identity was not tied to the `ServiceUser` owner. The
  factory now checks `requester == m_userIdentity`; the changed source and call path were
  re-reviewed. No other control issue was found.
- **Compile/build misses**: the first build of this binding slice used invalid const
  pybind holders and lacked complete headers for `NativeAuthenticatedGrantClient` and
  `NativeRequestRuntime`. The build exposed these errors; non-const holders and full native
  headers were added before the corrected build. The corrected extension build exited 0.
- **Runtime/test misses**: the first catalog binding accepted a Python `bytes` value only
  after the lambda was changed from `std::vector<uint8_t>` to `py::bytes`; the first facade
  test targeted the wrong public class. Both were corrected and the scoped suites were rerun.
  No native request parity or Provider failure result was inferred from these tests.

## Build and behavior result

The corrected command was:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  NDNSF_LIBRARY_DIR=/home/tianxing/NDN/ndn-service-framework/.codex-tmp/spec182-r4-b2/build \
  NDNSF_NAC_ABE_PREFIX=/home/tianxing/NDN/nac-abe-integration-182/install \
  NDNSF_NDN_SVS_SOURCE_TREE=/home/tianxing/NDN/ndn-svs \
  NDNSF_NDN_SVS_BUILD_TREE=/home/tianxing/NDN/ndn-svs/build \
  python3 setup.py build_ext --inplace --force
```

Observed wall time across the build invocation was approximately 197 seconds (one
extension target, system-first compiler/linker, unchanged candidate source closure); this
is a single observation, not a speedup claim. The follow-up checks were:

```text
PYTHONPATH=pythonWrapper python3 -m pytest -q \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_closure.py \
  tests/python/test_ndnsf_python_service_response_binding.py
result: 24 passed in 0.38s

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_spec182_native_closure.py
result: 32 passed in 1.11s

python import and invalid-catalog negative: PASS
```

The batch therefore records `STATIC_PASS`, `BUILD_PASS`, and focused
`FOCUSED_BEHAVIOR_PASS` for its binding/facade members, while the task state remains
`PARTIAL` and the feature is not `QUALIFICATION_PASS`.

## Remaining

Next work is a separate caller-migration batch: construct a real operator-pinned runtime,
exercise one native request through the public facade against the maintained Provider path,
then migrate maintained callers and retire the old default route only after parity and
T013/T016 gates are met. The shared Spec Kit batch rules and task-progress registry already
require this stable-exit, five-lane, and miss-taxonomy record for future Specs.
