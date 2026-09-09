# R5-B7 Native Stream Observer Facade

## Status

`STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL`。

本批只增加 native handle 的 Python 观察边界：`NativeInferenceHandle::observe` 继续由 C++
维护事件队列、replay 顺序、终态和异常隔离；pybind 将 request id、bytes payload 和
terminal 标志转换为短生命周期 Python 字典；`APPClient.request_native_payload` 在提交后
挂载可选 `on_event`，并在提交前拒绝不可调用对象。未新增 Python planner、strategy 或协议
状态 owner。真实 Provider callback、跨进程 token stream、conversation owner 和 T016 仍未
完成，因此本批不宣称完整 streaming qualification。

## Static review

按官方 `$review-agent` 只读 defect-first 方法检查了：

- `NativeInferenceHandle::observe`/`publishEvent` 的锁、通知队列、late replay、terminal
  和 callback exception isolation；
- `bindDistributedInference` 的 pybind holder、GIL acquisition、bytes conversion 与
  observer capture lifetime；
- `APPClient.request_native_payload` 的 callable validation、native request 提交顺序和
  无 observer 兼容路径；
- Python source gates、C++ selector 注册、shared target 与 extension source closure。

首轮构建发现绑定 lambda 将 handle 作为 `const` 接收，而 C++ `observe` 是非 const 方法；
已改为非 const 引用并完成复审。测试门原先把所有 `py::function` 视为 planner 违规，已
收窄为允许明确的 observer facade，同时继续拒绝 Python strategy/trampoline 和 legacy
imports。未发现其他可执行缺陷。

## Verification

```text
git diff --check
-> exit 0

/usr/bin/python3 -m py_compile \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_bindings.py
-> exit 0

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  /usr/bin/python3 -m pytest -q \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
-> 28 passed

/usr/bin/time build-nac182/unit-tests \
  --run_test=Spec182ClientState/SlowObserverDoesNotBlockCancelAndLateReplaySurvivesClientClose
-> exit 0; elapsed=0.22s

/usr/bin/time build-nac182/unit-tests \
  --run_test=Spec182ClientState/RealDeadlineDoesNotWaitForWorkOrSlowObserver
-> exit 0; elapsed=0.18s

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference /usr/bin/python3 - <<'PY'
from ndnsf import _ndnsf
assert hasattr(_ndnsf.NativeInferenceHandle, "observe")
print("handle_observe=True")
PY
-> exit 0; handle_observe=True
```

Native shared target and Python extension were rebuilt with the system-first toolchain. The first
extension attempt failed at compile boundary because of the const mismatch above; the corrected
forced rebuild exited 0 and linked `build-nac182` plus the pinned NAC-ABE prefix. A timed repeat of
the same forced source-closure build recorded `elapsed=335.26s`, exit 0. During that large single
translation-unit build, `vmstat 1` showed intermittent swap-in/out and available memory below
0.3 GB; the next independent native build must use `-j2` if that pressure persists.

## Coverage matrix

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry / callers | covered | `NativeInferenceHandle::observe`; `APPClient.request_native_payload(on_event=...)`; maintained Qwen route remains the caller |
| implementation and wire | covered | `NativeInferenceClient.cpp` observer queue; `pythonWrapper/src/ndnsf/di_bindings.cpp`; Python callback validation and bytes conversion |
| test / harness / oracle | covered for native observer semantics and facade/source shape; gap for Python callback delivery through a real Provider | C++ `Spec182ClientState` observer/deadline selectors; 28 Python focused cases; export smoke check |
| build / source closure | covered | Waf shared DI target and forced `_ndnsf` extension; system-first `/usr/bin/g++` and explicit library prefixes |
| migration / evidence | PARTIAL | `tasks.md` R5-B7 row and this record; T013-A, YOLO/Provider migration and T016 remain open |

## Result classification

| Category | Result |
| --- | --- |
| Static findings | const handle mismatch and over-broad test assertion; both repaired and re-reviewed |
| Compile/build misses | first extension compile caught the const mismatch; no linker or source-closure miss after correction |
| Runtime/test misses | none after correction; real Provider callback delivery remains unobserved |
| Build measurement | shared DI target exit 0, 2.542s; timed forced extension source-closure build exit 0, elapsed=335.26s; system-first `/usr/bin/g++` 9.4.0 |
| Behavior result | `STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL`; not `QUALIFICATION_PASS` |

## Remaining boundary

The observer facade is a bounded observability capability, not native streaming execution. Next work
must either exercise the callback on a real Core/Provider request or implement the missing native
streaming/conversation owner, then continue maintained YOLO/Provider migration and T016.
