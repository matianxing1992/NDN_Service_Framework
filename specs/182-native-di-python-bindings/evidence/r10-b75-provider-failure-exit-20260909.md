# R10-B75 Provider Failure Exit and CLI Parser Repair — 2026-09-09

本批承接 R10-B74 静态审计，只修复已确认的 Provider 失败退出和重复 CLI 分支。
没有推进 P1–P7，也没有把 standalone serve 探针当作 requester、跨进程或资格验收。

## Changes

- 删除 `examples/DI_NativeProviderExecutable.cpp` 中重复的 `--bootstrap-token` 分支，
  保留唯一解析入口。
- Provider `--serve` 的 detached provisioning task 现在发布失败状态并等待自身完成；
  失败时停止 Face `io_context`，主线程取消 NDNSD heartbeat scheduler 后以 rc=2 返回。
  这样既保留了正常 serving 所需的事件循环，又避免失败 Provider 永久存活。
- `ServiceProvider::stopNdnsdPeriodicPublish()` 只在 Face 事件线程完成 scheduler 取消，
  以避免 NDNSD scheduler 与事件回调并发访问。

## Verification

静态检查：

```text
cppcheck --enable=warning,performance,portability --inconclusive --std=c++17 \
  --language=c++ --quiet examples/DI_NativeProviderExecutable.cpp
-> rc=0; no diagnostic emitted

provider_static_contract=PASS
bootstrap_token_handlers=1
git diff --check=PASS
```

构建：

```text
./waf -o build-nac182 build --targets=DI_NativeRequester,di-native-provider -j4
-> first attempt failed at ServiceProvider.cpp because ScopedEventId has no reset()

./waf -o build-nac182 build --targets=DI_NativeRequester,di-native-provider -j2
-> PASS, 1m20.055s; requester and Provider linked from the current source tree
```

第一次 `-j4` 编译失败的原始边界见
`.codex-tmp/spec182-r10-b75-provider-failure-exit-20260909-r0-build-j4-fail/`；
修正为 move-assignment 后才重试。构建期间主机第二个 `vmstat` 样本出现
`si=1024/so=840`，因此后续重试按本机资源规则降为 `-j2`。

行为探针使用 metadata-only manifest、system-first runtime PATH 和无 Controller 权限的
独立 Provider：

```text
timeout --kill-after=1s 12s di-native-provider ... --serve \
  --disable-tokens --tracer-deterministic-runner --permission-wait-ms 1000 \
  --offer-backend onnxruntime-cpu
-> rc=2, elapsed=1.08s
-> SERVE_READY, EVENT_LOOP_EXCEPTION (public parameters),
   PROVISION_FAILED (provider permission not installed) observed
```

同一边界在修复前连续以 timeout `124` 收尾；修复后 Provider 产生明确非零退出，
没有声明请求/响应或 Controller 资格结果。`--check-only --tracer-deterministic-runner`
也通过并输出 `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`（4 roles、4 artifacts、4 registered）。

Core/host 回归：

```text
unit-tests --run_test='Spec182ProviderHost/*' -> 7/7 cases, no errors
integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182R10B*'
  -> 4/4 cases, 15 assertions, no errors, 13.978s
```

这些回归使用当前候选 shared library，覆盖已有 in-process Provider host/unary/stream/Qwen
边界；它们不改变 standalone worker、跨进程、no-Python 或 T016 的状态。

## Status

`STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for Provider failure observability;
`OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS`.

Remaining: independent requester → Core → Provider transport, continuation/recovery,
maintained native caller migration, legacy zero-use, I02–I08 and T016/T017.
