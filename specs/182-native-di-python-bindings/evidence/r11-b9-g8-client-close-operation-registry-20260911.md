# R11-B9-G8 Client Close Pending-Operation Registry

本批修复 `NativeInferenceClient` 的一个 C++ 生命周期缺口：原实现的
`m_operations` 只有 weak entries。调用方丢弃 public `NativeInferenceHandle` 后，
queued preparation callback 仍由 executor 持有 operation；此时 `close()` 看不到该
operation，可能在 client 已关闭后继续进入 adapter/preparation。

## Scope and implementation

| Lane | Result |
| --- | --- |
| production entry/callers | `NativeInferenceClient::request()`、`markTerminal()`、`NativeInferenceClient::close()` |
| implementation/wire | `NativeOperationRegistry` 以 strong `shared_ptr` 保留 pending operations；terminal transition 注销；`close()` 在 client mutex 与 registry mutex 下摘出后统一调用 `cancelOperation()` |
| ownership/concurrency | operation 对 registry 仅 weak reference，避免 client/operation 循环；request/close 使用 `client -> registry` 顺序，terminal 使用 `operation -> registry` 顺序，没有 registry-to-client 反向锁；已完成 operation 不留在 strong registry |
| test boundary | 新增 handle drop 后 `close()` 的 C++ regression；保留 19-case `Spec182ClientState` suite |
| qualification boundary | 仅关闭 client close ownership boundary；完整 lifecycle、maintained callers、legacy zero-use、no-Python、T015/T016 与 T017 仍开放 |

## Static review

按 `review-agent` 的 defect-first 只读方法检查完整 diff、调用方和测试。CodeGraph
复核 `request()` → executor、`markTerminal()` → `unregisterOperation()` 以及
`close()` → `cancelOperation()` 路径；检查了锁顺序、终态注销、queued callback
和 client/operation 所有权。没有发现新增的可行动缺陷。

Changed C++ files were also checked with `cppcheck` using the system include paths.
The command returned `rc=0`, but emitted the existing vendored nlohmann header syntax
diagnostic (`cpp/vendor/nlohmann/json.hpp:3409`) and a too-many-configurations info
message; this is recorded as a tool limitation, not a clean whole-tree static proof.

## Validation

First `-j3` attempt reached the `di-native-client.t.cpp` translation unit but failed
because the new fixture tried to `dynamic_pointer_cast` from
`shared_ptr<const NativeModelAdapter>` to a non-const concrete adapter. No production
runtime was reached. The raw first-boundary output is retained at
`.codex-tmp/spec182-g49-close-registry-20260911/build-j3.log`.

The fixture was narrowed to retain the concrete adapter at registration. The retry used
the system-first compiler/binutils closure and `-j3`:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=unit-tests -j3 -v
```

The fresh retry completed all 190/190 unit-test build tasks in 57.934 seconds (`rc=0`).
Recorded `vmstat` samples had zero sustained `si`/`so`; the host did not cross the
swap boundary that forced the earlier `-j4` attempt down to a safer setting.

The focused and affected-suite outputs are retained under
`.codex-tmp/spec182-g49-close-registry-20260911/`:

```text
Spec182ClientState/ClientCloseCancelsPendingOperationAfterHandleDrop
  1 case, 3 assertions: PASS

Spec182ClientState/*
  19 cases, 376 assertions: PASS

*Spec182* selector on the same binary
  260 cases, process exit rc=0
```

`git diff --check` passed. This checkpoint does not promote any Spec182 parent task;
it supplies one verified prerequisite for T010-A and the remaining native closure work.
