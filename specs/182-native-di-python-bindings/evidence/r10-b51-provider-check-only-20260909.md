# R10-B51 Provider Check-Only With Real ONNX Bundle 2026-09-09

## Batch allocation

本批只验证已经链接闭合的 standalone Provider 在真实四角色 ONNX bundle 上能否加载、
预热并注册全部角色。共同入口是 `examples/DI_NativeProviderExecutable.cpp` 的
`--check-only` 分支；稳定出口是 Provider 自己打印 `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`
并输出带角色、artifact digest、plan digest 和 runtime 版本的 execution evidence。
该出口不启动 NDN Face、Provider `--serve`、requester 或跨进程 transport，因此不吸收
T010/T013/T016 的完整资格门。

## Coverage matrix

| Lane | Covered files/symbols and check | Result |
| --- | --- | --- |
| production entry/callers | `di-native-provider` → `parseArgs` → `loadPlan`/`loadManifestSpecs` → `RegistryNativeModelRunnerFactory` → `NativeProviderSession::registerRunner` | covered |
| implementation/wire | `examples/DI_NativeProviderExecutable.cpp` check-only branch; four roles `/Backbone`, `/Head/Shard/0`, `/Head/Shard/1`, `/Merge`; DATA_DRIVEN_V2 plan | covered |
| test/harness/oracle | Existing bundle `spec174-exact-bundle-gpu-v5`; process exit, `NDNSF_DI_EXECUTION_EVIDENCE`, and `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK` assertions | covered |
| build/source closure | source commit `7b0e70268b77b117d647b45890a5453859a7b6a8`; Provider binary SHA below; `ldd` reports zero `not found` | covered |
| migration/evidence | Fresh raw run directory, exact command/log/result, artifact and plan digests retained; no Python runtime or maintained caller invoked | covered |

## Static review

只读 review 检查了参数互斥、service/role 对齐、manifest 相对路径工作目录、source
closure、runner evidence 聚合及 CLI 输出。未发现 P1/P2/P3；没有编译漏检或运行漏检。
`--help` 不属于本批契约，既有 CLI 仍返回 usage/exit `2`，未被当作产品失败。

## Review trace

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）执行只读审查；
基线为 source commit `7b0e70268b77b117d647b45890a5453859a7b6a8`，完整审查范围是本批
Provider 可执行文件的已链接产物、`DI_NativeProviderExecutable.cpp` 相关入口/调用链、
四角色 plan/manifest 和本证据/进度更新。审查逐项核对 production entry/callers、
implementation/wire、test/harness/oracle、build/source closure、migration/evidence，
并复查 selector/输出契约；结论为 `No findings`。

## Command and result

工作目录为 `.codex-tmp/spec174-exact-bundle-gpu-v5`，使用已修复的 Provider binary：

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
  .codex-tmp/spec182-r4-b2/build/examples/di-native-provider \
  --plan native-execution-plan.json \
  --manifest service-manifest.json \
  --service /Inference/NativeTracer \
  --provider /example/native-provider \
  --roles all --execution-policy DATA_DRIVEN_V2 --check-only
```

The command exited `0` in `0.05s`. The log contains:

```text
NDNSF_DI_NATIVE_PROVIDER_PLAN_READY roles=4 artifacts=4 activeRoles=4 runners=4
NDNSF_DI_EXECUTION_EVIDENCE ... runnerKind=onnxruntime-cpu ... runtimeVersion=1.26.0 ...
  roles=[/Backbone,/Head/Shard/0,/Head/Shard/1,/Merge] ...
  loadCompleted=true warmupCompleted=true cpuFallbackUsed=false
NDNSF_DI_NATIVE_PROVIDER_CHECK_OK service=/Inference/NativeTracer roles=4 artifacts=4 registered=4 workers=1
```

Provider binary SHA-256 is
`4be6b29acb10b29792757a23ce0ffc4f098f39e9cec2f0f47f4175d03bdae75a`; `ldd` found no unresolved
dependencies. The raw output is retained in
`.codex-tmp/spec182-r10-b51-provider-check-only-20260909/provider-check-only.log` and
`result.txt`.

## Closure decision

`CLOSED_FOR_VALIDATION` for real ONNX Provider load/warmup/registration in check-only mode.
`OPEN_FOR_NEXT_BATCH` for Provider `--serve`, independent requester/Provider transport,
terminal native Response, maintained caller/no-Python migration, and T016 qualification.
This is a focused Provider readiness result and does not close T010-B or any parent task.
