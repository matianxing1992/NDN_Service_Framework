# R10-B77 Provider relocatable RUNPATH boundary

本批只修复独立 `di-native-provider` target 的动态加载边界，并为该接线增加
一个 Python 静态回归断言。它不声称 Provider 已完成真实请求、跨进程 transport
或 T016 qualification。

| lane | result |
| --- | --- |
| production entry/callers | `examples/wscript` → `di-native-provider` target；仅修改该 target 的 link flags |
| implementation/wire | Provider target 现在显式携带 `-Wl,-rpath,$ORIGIN/..`，与 requester/native smoke target 使用同一 staged-library lookup fallback |
| test/harness/oracle | `test_native_provider_target_has_relocatable_origin_runpath` 通过；未启动 Provider 或网络 harness |
| build/source closure | system-first Waf target build `di-native-provider`, 90/90, exit 0, 7.270 s；artifact SHA `sha256:7bd314a551c6ec6c31b5c4b8c216e50bd9f4b30734d4685082dcb5a4faa2b11e` |
| migration/evidence | `readelf -d` 观察到 `$ORIGIN/..`；当前 build 仍保留 NAC-ABE/SVS 的绝对 runpath 和 host-installed `ndnsd/openabe/relic`，所以只关闭 staged lookup 前置，不关闭完整部署闭包 |

## Commands

```text
python3 -m py_compile examples/wscript tests/python/test_spec182_native_bindings.py
python3 -m pytest -q tests/python/test_spec182_native_bindings.py -k native_provider_target_has_relocatable_origin_runpath
1 passed, 16 deselected
env PATH=/usr/bin:/bin:/usr/sbin:/sbin ./waf -o .codex-tmp/spec182-p1-provider-rpath/build build --targets=di-native-provider -j2
'build' finished successfully (7.270s)
readelf -d .codex-tmp/spec182-r4-b2/build/examples/di-native-provider
Library runpath: [/home/tianxing/NDN/nac-abe-integration-182/install/lib:/home/tianxing/NDN/ndn-svs/build:$ORIGIN/..]
```

`git diff --check` passed. The next P1 gate must stage the complete shared-library
closure and launch independent requester/provider processes; this batch remains
`CLOSED_FOR_VALIDATION` only for the Provider RUNPATH wiring.

## Review trace

Read-only `$review-agent` review used SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `6787677d`.
The diff, target registration, and focused regression were checked; no actionable P1/P2/P3
finding was found. The residual absolute runtime paths are recorded above as a deployment
closure gap rather than hidden by the new fallback flag.
