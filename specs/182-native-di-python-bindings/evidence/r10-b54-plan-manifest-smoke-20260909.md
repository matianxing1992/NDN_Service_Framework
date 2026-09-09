# R10-B54 Native Plan and Manifest Smoke 2026-09-09

## Batch allocation

本批沿 R10-B53 的本地原生 plan/session 出口，验证另一条独立的 manifest 解析、角色注册和
依赖发布入口。共同入口是 `di-native-plan-manifest-smoke`；稳定出口是
`NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK`。输入复用
`.codex-tmp/spec174-exact-bundle-gpu-v5` 的四角色 plan/manifest，仍不启动 NDN Face、
Provider `--serve`、Python 或跨进程 requester。

## First failure boundary and changed gate

第一次构建在 55/55 编译任务后链接失败，未解析符号同时覆盖 native ONNX planning/recipe
helpers 和 `ServiceUser` publication/collaboration APIs；没有 smoke 进程或协议请求开始。
完整 linker 输出保留在 `.codex-tmp/spec182-r10-b54-plan-manifest-smoke-20260909/build.log`。

该边界是 target source/link closure 漏项，不是 plan、manifest 或协议行为结果。修复前建立
project-symbol definition map：

| Unresolved group | Defining translation units | Providing target/library |
| --- | --- | --- |
| `inspectNativeOnnxPlanningGraph`, `canonicalOnnxSourceIdentity`, `inspectNativeOnnxSourceGraph`, `checkOnnxAssemblerDescriptorBinding` | `NativeOnnxRecipeAssembler.cpp` | `di-native-plan-manifest-smoke` source closure |
| `canonicalNativeOnnxRecipeJson` | `NativeOnnxAssemblyWorker.cpp` | `di-native-plan-manifest-smoke` source closure |
| `ServiceUser::*` publication, collaboration and IO methods | `ndn-service-framework/ServiceUser.cpp` and candidate framework build | `ndn-service-framework` shared library |

`examples/wscript` 随后加入 `di_native_onnx_assembly_sources`、`ndn-service-framework`、
ONNX/Protobuf link closure 以及 `$ORIGIN/..` RUNPATH。该 map 由精确 `rg`/CodeGraph 查询、
`nm -D -C --defined-only` 和 `readelf -d` 复核；这是针对首次链接漏检新增的 `Changed gate`。

## Coverage matrix

| Lane | Covered files/symbols and check | Result |
| --- | --- | --- |
| production entry/callers | `DI_NativePlanManifestSmoke.cpp::main` → plan/manifest loaders → native role registration and dependency publication | covered |
| implementation/wire | `NativeExecutionPlanJson`, `NativeServiceManifest`, `NativeProviderSession`, `NativeCanonical*` and ONNX recipe helpers | covered |
| test/harness/oracle | Four-role `spec174-exact-bundle-gpu-v5` plan/manifest; marker plus role/artifact/output-tensor counts; target registration checked in `examples/wscript` | covered |
| build/source closure | Waf target source/link list, definition map above, retry target output, candidate framework exports, RUNPATH and default `ldd` path | covered |
| migration/evidence | First link boundary and raw logs retained; no Python/network process; smoke result and hashes recorded | covered |

## Review trace

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）执行只读审查；基线为
source checkpoint `ea09e05a`，范围覆盖 `DI_NativePlanManifestSmoke.cpp`、plan/manifest
loaders、Provider session registration/publication、`examples/wscript` target/source/link
closure、candidate framework exports/RUNPATH、bundle artifact cwd 和 smoke assertions。
首次链接漏项被单列为 compile/link miss；definition map 与 target registration 修复后复审，
没有新的 P1/P2/P3。

## Verification

```text
./waf -o .codex-tmp/spec182-r4-b2/build build \
  --targets=di-native-plan-manifest-smoke -j2
-> initial exit 1 after 55/55 tasks; elapsed 133.41s; unresolved source/link closure

./waf -o .codex-tmp/spec182-r4-b2/build build \
  --targets=di-native-plan-manifest-smoke -j2
-> retry exit 0; 85/85 tasks; elapsed 170.09s

readelf -d di-native-plan-manifest-smoke
-> RUNPATH includes $ORIGIN/..; ldd without LD_LIBRARY_PATH selects the candidate
   build-tree libndn-service-framework.so.0.1.0; no `not found`

(cwd=.codex-tmp/spec174-exact-bundle-gpu-v5)
di-native-plan-manifest-smoke native-execution-plan.json service-manifest.json /Inference/NativeTracer
-> exit 0; elapsed 0.06s
NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK roles=4 artifacts=4 outputTensors=8
```

The smoke binary SHA-256 is
`2d9e1ed4b46f82fb1b03c2ed65f309e85c767f4232d05120d8def669a295ca1c`; the candidate framework
library SHA-256 is `e87e27d0f485020088fa428b4bc834a24d742ca560ec87fa7db954003fdc8865`.
All raw build, loader and smoke outputs are under
`.codex-tmp/spec182-r10-b54-plan-manifest-smoke-20260909/`.

## Batch retrospective

- `static`: 首次静态门未提前发现 manifest target 的 ONNX/framework source closure 缺口；修复
  后的 definition-map、target registration、导出符号和 RUNPATH 复审没有新控制性问题。
- `compile/link`: 首个失败边界是 55/55 后的 linker unresolved symbols；map 明确映射到定义
  translation units 和 shared target，改变门禁后 retry 成功。
- `runtime/test`: 没有运行阶段漏检；修复后的本地 smoke 从 bundle 根目录得到稳定 marker。该
  结果仍未观测 Provider `--serve`、真实 requester/Provider transport 或 terminal Response。
- `unobserved`: maintained caller/no-Python migration、跨进程请求、conversation/recovery、
  MiniNDN 和 T016 qualification 仍未观测；target/link smoke 不覆盖这些义务。

本批在稳定出口出现后立即停止扩张，没有继续吸收 requester、Provider serve 或资格成员；
`-j2` 是因前一轮 unit 后观察到 swap-in 的资源约束，不能据此推导整体提速。

## Closure decision

`CLOSED_FOR_VALIDATION` for local native plan/manifest parsing, four-role registration,
dependency publication and output-tensor accounting under the candidate library closure.
`OPEN_FOR_NEXT_BATCH` for Provider `--serve`, independent requester/Provider transport,
authenticated terminal Response, maintained caller/no-Python migration and T016 qualification.
This focused result does not close T010-B or any parent task.
