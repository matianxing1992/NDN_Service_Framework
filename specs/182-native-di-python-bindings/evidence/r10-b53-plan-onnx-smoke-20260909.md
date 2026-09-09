# R10-B53 Native Plan and ONNX Session Smoke 2026-09-09

## Batch allocation

本批把 R10-B50 的 Provider source closure 推进到本地原生 plan/session 行为：共同入口是
`di-native-plan-onnx-smoke`，它直接解析四角色 plan/manifest、创建 ONNX Runtime runner、
执行 `NativeProviderSession` 并检查依赖发布和最终输出。稳定出口是
`NDNSF_DI_NATIVE_PLAN_ONNX_SMOKE_OK`；不启动 NDN Face、Provider `--serve` 或 Python。

## First failure boundary and changed gate

86/86 个编译/链接任务先成功，但第一次运行在进程启动阶段以 `rc=127` 失败：
`/usr/local/lib/libndn-service-framework.so.0.1.0` 缺少
`ServiceUser::publishSignedAppData`，尚未解析 plan。该边界保留在
`.codex-tmp/spec182-r10-b53-plan-onnx-smoke-20260909/smoke.log`，不是模型或协议结果。

按 failure-log 登记的改变门禁，候选 build-tree framework library 导出该符号，随后用显式
`LD_LIBRARY_PATH` 运行；第二次尝试越过 loader 后因从仓库根启动而找不到 manifest 的相对
artifact 路径（`rc=2`），输出保留在 `smoke-candidate-lib.log`。最终从 bundle 根目录启动，
并用 `LD_LIBRARY_PATH` 复查 `ldd` 选中候选 framework library。

## Coverage matrix

| Lane | Covered files/symbols and check | Result |
| --- | --- | --- |
| production entry/callers | `DI_NativePlanOnnxSmoke.cpp::main` → `loadPlan`/`loadManifestSpecs` → `NativeProviderSession::executeRoleAsync` | covered |
| implementation/wire | `NativeExecutionPlanJson`, `NativeServiceManifest`, ONNX Runtime runner, dependency I/O and final output scopes | covered |
| test/harness/oracle | Existing `spec174-exact-bundle-gpu-v5` four-role plan/manifest; smoke marker, dependency count and output-byte assertions | covered |
| build/source closure | Waf target 86/86 with system-first `-j2`; candidate framework/binary hashes and loader path recorded | covered |
| migration/evidence | First loader and cwd boundaries retained; final command/log/result stored; no Python or network process | covered |

## Review trace

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）执行只读审查；
基线为 source commit `9a69e847cf9385a80e4bfde75ea22c2184740db3`，完整范围覆盖 smoke
入口、plan/manifest loaders、session execution、Waf target registration、runtime loader
identity 和 bundle-relative artifact contract。五 lane 检查无 P1/P2/P3；两次运行边界均在
重试前写入 failure-log。

## Verification

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
  ./waf -o .codex-tmp/spec182-r4-b2/build build \
  --targets=di-native-plan-onnx-smoke -j2
-> exit 0; 86/86 tasks; elapsed 153.14s

LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:... \
  .codex-tmp/spec182-r4-b2/build/examples/di-native-plan-onnx-smoke \
  native-execution-plan.json service-manifest.json /Inference/NativeTracer
  (cwd=.codex-tmp/spec174-exact-bundle-gpu-v5)
-> exit 0; elapsed 0.05s
NDNSF_DI_NATIVE_PLAN_ONNX_SMOKE_OK roles=4 artifacts=4 dependencyObjects=4 encodedBundleOutputs=3 outputBytes=440
```

The smoke binary SHA-256 is
`ed6825008ba23af86c0c158896af42002d2d7b3a2a2025c0553ada7bef29bbbe`; the candidate framework
library SHA-256 is `e87e27d0f485020088fa428b4bc834a24d742ca560ec87fa7db954003fdc8865`.
With the explicit library path, `ldd` resolves `libndn-service-framework.so.0.1.0` to the
candidate build tree and reports no `not found`. Raw logs and result files are under
`.codex-tmp/spec182-r10-b53-plan-onnx-smoke-20260909/`.

## Closure decision

`CLOSED_FOR_VALIDATION` for local native plan parsing, four-role ONNX session execution,
dependency publication and final output under the candidate library closure.
`OPEN_FOR_NEXT_BATCH` for packaging the runtime library identity/RUNPATH, Provider `--serve`,
independent requester/Provider transport, authenticated terminal Response, maintained caller/
no-Python migration and T016 qualification. This smoke result does not close T010-B or any parent
task.
